#include <unistd.h>
#include <fcntl.h>

#include <iostream>
#include <stdint.h>
#include <chrono>
#include <tuple>
#include <vector>
#include <string_view>
#include <charconv>
#include <map>
#include <algorithm>

#ifdef USING_WIRING_PI
#include <wiringPi.h>
#endif

using namespace std::string_view_literals;
using namespace std::chrono_literals;

using duration_t  = std::chrono::milliseconds;
using timepoint_t = std::chrono::high_resolution_clock::time_point;
using seconds_t   = std::chrono::duration<double, std::ratio<1, 1>>;
using arguments_t = std::vector<std::string_view>;

auto now() noexcept {
    return std::chrono::high_resolution_clock::now();
}

template<typename T>
double to_sec(T const &d) noexcept {
    return std::chrono::duration_cast<seconds_t>(d).count();
}

template<typename T>
T multiply_duration(T const &d, double ratio) noexcept {
    return T(static_cast<typename T::rep>(d.count() * ratio));
} 

// structure from the USBMon Device
//DIRECT copy from https://www.kernel.org/doc/Documentation/usb/usbmon.txt
struct [[gnu::packed]] usbmon_packet {
    uint64_t      id;               /*  0: URB ID - from submission to callback */
    unsigned char type;             /*  8: Same as text; extensible. */
    unsigned char xfer_type;        /*    ISO (0), Intr, Control, Bulk (3) */
    unsigned char epnum;            /*     Endpoint number and transfer direction */
    unsigned char devnum;           /*     Device address */
    uint16_t      busnum;           /* 12: Bus number */
    char          flag_setup;       /* 14: Same as text */
    char          flag_data;        /* 15: Same as text; Binary zero is OK. */
    int64_t       ts_sec;           /* 16: gettimeofday */
    int32_t       ts_usec;          /* 24: gettimeofday */
    int           status;           /* 28: */
    unsigned int  length;           /* 32: Length of data (submitted or actual) */
    unsigned int  len_cap;          /* 36: Delivered length */
    union {                         /* 40: */
        unsigned char setup[2];     /* Only for Control S-type */
        struct iso_rec {            /* Only for ISO */
            int error_count;
            int numdesc;
        } iso;
    } s;
    int          interval;          /* 48: Only for Interrupt and ISO */
    int          start_frame;       /* 52: For ISO */
    unsigned int xfer_flags;        /* 56: copy of URB's transfer_flags */
    unsigned int ndesc;             /* 60: Actual number of ISO descriptors */
};                                  /* 64 total length */


// Configuration change defaults here
struct Config {
    bool       logging           = false;
    uint64_t   max_transfer_rate = 10 * 1024 * 1024;
    uint64_t   min_transfer_rate = 0;
    duration_t pwm_periode       = 100ms;
    double     off_periode_ratio = 0.1;
    int        led_gpio_pin      = 0;

    friend std::ostream& operator<<(std::ostream &s, Config const &cfg) {
        s << "\nConfiguration:\n";
        s << "-logging: " << std::boolalpha << cfg.logging << '\n';
        s << "-period: " << to_sec(cfg.pwm_periode) << " s\n";
        s << "-off_period_ratio: " << cfg.off_periode_ratio * 100.0 << "%\n";
        s << "-max_transfer_rate: " << cfg.max_transfer_rate / 1024.0 << " kbps\n";
        s << "-min_transfer_rate: " << cfg.min_transfer_rate / 1024.0 << " kbps\n";
        s << "-gpio: " << cfg.led_gpio_pin << '\n';
        return s;
    }

    void calculate_periode_values() noexcept {
        max_transfer_rate *= to_sec(pwm_periode);
        min_transfer_rate *= to_sec(pwm_periode);
    }

    // calculate the high and low duration of the led based on the settings
    std::pair<duration_t, duration_t> calculate_durations(uint64_t bytes) const noexcept {
        auto clamped   = std::clamp(bytes, min_transfer_rate, max_transfer_rate);
        auto ratio     = static_cast<double>(clamped - min_transfer_rate) / (max_transfer_rate - min_transfer_rate);
        auto on_ration = ratio * (1.0 - off_periode_ratio);
        return { 
            multiply_duration(pwm_periode, on_ration),
            multiply_duration(pwm_periode, 1.0 - on_ration)
        };
    }
};

class UsbMon {
    int fd;
    uint64_t accumulated_bytes;
public:
    UsbMon() 
    : fd { -1 }
    , accumulated_bytes{ 0 }
    {
        fd = open("/dev/usbmon0", O_RDONLY | O_NONBLOCK);
        if (fd == -1) {
            std::cerr << "Cannot open usbmon device! forgot sudo?\n";
            std::exit(-1);
        }
    }
    ~UsbMon() {
        close(fd);
    }
    // sum all the bytes over a given duration
    duration_t accumulate_bytes_for(duration_t const &dur) noexcept {
        auto tsc = now();
        while ( now() - tsc < dur ) {
            accumulated_bytes += get_transfered_bytes();
        }
        return std::chrono::duration_cast<duration_t>(now() - tsc);
    }
    uint64_t get_and_reset_accumulated_bytes() noexcept {
        return std::exchange(accumulated_bytes, 0);
    }
private:
    // get the transfered byte from the last packet
    uint64_t get_transfered_bytes() const noexcept {
        usbmon_packet p;
        ssize_t n = read(fd, &p, sizeof(usbmon_packet));
        if (n != 48) return 0; // lagacy read only returns 48 bytes and not sizeof(usbmon_packet)
        if (p.type != 'S' && p.type != 'C') return 0; // only accumulate the CALLBACK type
        return p.length;
    }
};

struct Raspi {
    enum class LedState { On, Off};
    static void set_led_state(Config const &cfg, Raspi::LedState state) {
    #ifdef USING_WIRING_PI
        if (state == LedState::On) {
            digitalWrite(cfg.led_gpio_pin, HIGH);
        } else {
            digitalWrite(cfg.led_gpio_pin, LOW);
        }
    #endif
    }
};




// automatically generate pwm time based on the sample interval and the maximum transfer rate 
static void generate_led_pwm(Config const &cfg, UsbMon &monitor) {
    for (auto tsc = now(), last_tsc = tsc; ;last_tsc = tsc, tsc = now()) {
        auto accumulated_bytes = monitor.get_and_reset_accumulated_bytes();

        auto [duration_high, duration_low] = cfg.calculate_durations(accumulated_bytes);

        Raspi::set_led_state(cfg, Raspi::LedState::On);
        auto duration_high_real = monitor.accumulate_bytes_for(duration_high);
        Raspi::set_led_state(cfg, Raspi::LedState::Off);
        auto duration_low_real = monitor.accumulate_bytes_for(duration_low);
 
        if (cfg.logging) {
            printf(
                "Rate: %9.3f kb/s   PWM: %6.3f s   [H: %6.3f s   L:%6.3f s]\n", 
                accumulated_bytes / to_sec(cfg.pwm_periode) / 1024.0, 
                to_sec(tsc - last_tsc), 
                to_sec(duration_high_real),
                to_sec(duration_low_real)
            );
        }
    }
}

void print_help() {
    std::cout <<
        "-help                 ... print this message\n" \
        "-logging              ... enable logging\n" \
        "-period value[s,ms]   ... pwm period\n" \
        "-off value[%]         ... enforced off period of the led in percent\n" \
        "-max value[Mbps,kbps] ... maximum usb transfer rate\n" \
        "-min value[Mbps,kbps] ... minimum usb transfer rate\n" \
        "-gpio value           ... GPIO pin to use\n";
}

[[noreturn]] void unknown_argument_kill(std::string_view const &v) {
    std::cout << "Unknown or invalid argument: " << v << std::endl;
    print_help();
    std::exit(-1);
}

int64_t parse_int_argument_with_extent(std::string_view const &v, std::map<std::string_view, int64_t(*)(int64_t)> const &extentions) {
    int value = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), value);
    if (ec != std::errc{}) {
        unknown_argument_kill(v);
    }
    if (extentions.empty()) {
        return value;
    }
    std::string_view extention{ p };
    auto function = extentions.find(extention);
    if (function == extentions.end()) {
        unknown_argument_kill(extention);
    }
    return function->second(value);
}

arguments_t::const_iterator parse_argument(arguments_t::const_iterator begin, arguments_t::const_iterator end, Config &cfg) {
    auto N = std::distance(begin, end);
    if (N == 0) return end;

    auto const size_extentions = std::map<std::string_view, int64_t(*)(int64_t)>{
        {"Mbps"sv, [](int64_t v) { return v * 1024 * 1024; } },
        {"kbps"sv, [](int64_t v) { return v * 1024;        } }
    };

    if (*begin == "-logging"sv) {
        cfg.logging = true;
        return begin + 1;
    }
    if (*begin == "-period"sv && N >= 2) {
        cfg.pwm_periode = std::chrono::milliseconds(parse_int_argument_with_extent(*(begin+1), {
            { "s"sv, [](int64_t v) { return v * 1000; } },
            {"ms"sv, [](int64_t v) { return v * 1;    } }
        }));
        return begin + 2;
    }
    if (*begin == "-max"sv && N >= 2) {
        cfg.max_transfer_rate = static_cast<uint64_t>(parse_int_argument_with_extent(*(begin+1), size_extentions));
        return begin + 2;
    }
    if (*begin == "-min"sv && N >= 2) {
        cfg.min_transfer_rate = static_cast<uint64_t>(parse_int_argument_with_extent(*(begin+1), size_extentions));
        return begin + 2;
    }
    if (*begin == "-gpio"sv && N >= 2) {
        cfg.led_gpio_pin = static_cast<int>(parse_int_argument_with_extent(*(begin+1), {}));
        return begin + 2;
    }
    if (*begin == "-off"sv && N >= 2) {
        int64_t percent = parse_int_argument_with_extent(*(begin+1), {
            { "%"sv, [](int64_t v) { return v; } }
        });
        if (percent < 0 || percent > 100) {
            unknown_argument_kill(*(begin+1));
        }
        cfg.off_periode_ratio = percent / 100.0;
        return begin + 2;
    }
    if (*begin == "-help"sv) {
        print_help();
        return begin + 1;
    }

    unknown_argument_kill(*begin);
}

int main(int argc, char *argv[]) {
    arguments_t arguments;
    std::copy(argv, argv + argc, std::back_inserter(arguments));
    Config cfg{};
    for (auto current = arguments.cbegin() + 1; current != arguments.cend(); ) {
        current = parse_argument(current, arguments.cend(), cfg);
    }
    std::cout << cfg << std::endl;
    cfg.calculate_periode_values();

    UsbMon monitor{};
    
#ifdef USING_WIRING_PI
    wiringPiSetupGpio();
    pinMode(cfg.led_gpio_pin, OUTPUT);
#endif
    generate_led_pwm(cfg, monitor);
    return 0;
}
