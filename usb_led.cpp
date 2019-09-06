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

using namespace std;
using namespace std::string_view_literals;
using namespace std::chrono_literals;

using duration_t  = chrono::milliseconds;
using timepoint_t = chrono::high_resolution_clock::time_point;
using seconds_t   = chrono::duration<double, ratio<1, 1>>;
using arguments_t = vector<string_view>;

// get the current timepoint
timepoint_t now() noexcept {
    return chrono::high_resolution_clock::now();
}

// cvt any duration to seconds
template<typename T>
double to_sec(T const &d) noexcept {
    return chrono::duration_cast<seconds_t>(d).count();
}

// multiply a non double duration by a double
template<typename T>
T multiply_duration(T const &d, double ratio) noexcept {
    return T(static_cast<typename T::rep>(d.count() * ratio));
} 

timeval get_timeval_until(timepoint_t const &tp) {
    auto remaining = tp - now();
    auto remaining_s  = chrono::duration_cast<chrono::seconds>(remaining);
    auto remianing_us = chrono::duration_cast<chrono::microseconds>(remaining) 
        - chrono::duration_cast<chrono::microseconds>(remaining_s);

    timeval tv;
    tv.tv_sec  = remaining_s.count();
    tv.tv_usec = remianing_us.count();
    return tv;
}

// Configuration change defaults here
struct Config {
    bool       logging           = false;
    bool       invert            = false;
    uint64_t   max_transfer_rate = 10 * 1024 * 1024;
    uint64_t   min_transfer_rate = 0;
    duration_t pwm_periode       = 100ms;
    double     off_periode_ratio = 0.1;
    int        led_pin      = 17;

    // dump the current config
    void print() const noexcept {
        printf(
            "\nConfiguration:\n\t"
            "logging: %d \n\t"
            "periode: %.3f s\n\t"
            "off_period_ratio: %.0f %%\n\t"
            "max_transfer_rate: %.3f kbps\n\t"
            "min_transfer_rate: %.3f kbps\n\t"
            "pin: %d\n\t"
            "inverted: %d \n\n",
            logging,
            to_sec(pwm_periode),
            off_periode_ratio * 100,
            max_transfer_rate / 1024.0,
            min_transfer_rate / 1024.0,
            led_pin,
            invert
        );
    }

    // convert the transfer rates to the given periode
    void calculate_periode_values() noexcept {
        max_transfer_rate *= to_sec(pwm_periode);
        min_transfer_rate *= to_sec(pwm_periode);
    }

    // calculate the high and low duration of the led based on the settings
    pair<duration_t, duration_t> calculate_durations(uint64_t bytes) const noexcept {
        auto clamped   = clamp(bytes, min_transfer_rate, max_transfer_rate);
        auto ratio     = static_cast<double>(clamped - min_transfer_rate) / (max_transfer_rate - min_transfer_rate);
        auto on_ration = ratio * (1.0 - off_periode_ratio);
        return { 
            multiply_duration(pwm_periode, on_ration),
            multiply_duration(pwm_periode, 1.0 - on_ration)
        };
    }
};

class UsbMon {
    int      fd                = -1;
    uint64_t accumulated_bytes = 0;
    fd_set   waiting;
public:
    UsbMon() {
        fd = open("/dev/usbmon0", O_RDONLY);
        if (fd == -1) {
            cerr << "Cannot open usbmon device! forget sudo or modprobe? (\"sudo modprobe usbmon\") \n";
            exit(-1);
        }
        FD_ZERO(&waiting);
    }
    ~UsbMon() {
        close(fd);
    }
    // sum all the bytes over a given duration
    duration_t accumulate_bytes_for(duration_t const &dur) noexcept {
        auto tsc   = now();
        auto until = tsc + dur; 
        while ( now() < until ) {
            accumulated_bytes += get_transfered_bytes(until);
        }
        return chrono::duration_cast<duration_t>(now() - tsc);
    }
    // exchange the accumulated value with zero
    uint64_t get_and_reset_accumulated_bytes() noexcept {
        return exchange(accumulated_bytes, 0);
    }
private:
    // get the transfered byte from the last packet
    uint64_t get_transfered_bytes(timepoint_t const &until) noexcept {
        constexpr auto type_offset   = 8;
        constexpr auto length_offset = 32;
        unsigned char buffer[64];

        timeval tv = get_timeval_until(until);
        FD_SET(fd, &waiting);
        int ret = select(fd+1, &waiting, nullptr, nullptr, &tv);
        if (ret == -1 || ret == 0) 
            return 0;
        // lagacy read only returns 48 bytes
        if (read(fd, &buffer, 64) != 48) 
            return 0; 
        // only accumulate the CALLBACK type
        if (buffer[type_offset] != 'S' && buffer[type_offset] != 'C') 
            return 0; 
        return *reinterpret_cast<unsigned int*>(&buffer[length_offset]);
    }
};

class Raspi {
    int pin = 0;
    bool inverted = false;
public:
    Raspi(int p, bool inv) : pin{ p }, inverted{ inv } {
        #ifdef USING_WIRING_PI
            wiringPiSetupGpio();
            pinMode(pin, OUTPUT);
        #endif
    }
    enum class LedState { On, Off};
    void set_led_state(Raspi::LedState state) const noexcept {
        #ifdef USING_WIRING_PI
            if ((state == LedState::On) != inverted) {
                digitalWrite(pin, HIGH);
            } else {
                digitalWrite(pin, LOW);
            }
        #endif
    }
};

// automatically generate pwm time based on the sample interval and the maximum transfer rate 
static void generate_led_pwm(Config const &cfg, Raspi const &raspi, UsbMon &monitor) {
    timepoint_t tsc = now(), last_tsc = tsc;

    for(;;) {
        auto bytes_acc = monitor.get_and_reset_accumulated_bytes();

        auto [high, low] = cfg.calculate_durations(bytes_acc);

        raspi.set_led_state(Raspi::LedState::On);
        duration_t high_measured = monitor.accumulate_bytes_for(high);

        raspi.set_led_state(Raspi::LedState::Off);
        duration_t low_measured = monitor.accumulate_bytes_for(low);
 
        if (cfg.logging) {
            printf(
                "Rate: %9.3f kb/s   PWM: %6.3f s   [H: %6.3f s   L:%6.3f s]\n", 
                bytes_acc / to_sec(cfg.pwm_periode) / 1024.0, 
                to_sec(tsc - last_tsc), 
                to_sec(high_measured),
                to_sec(low_measured)
            );
        }
        last_tsc = std::exchange(tsc, now());
    }
}

void print_help() {
    puts(
        "-help                 ... print this message\n" \
        "-logging              ... enable logging\n" \
        "-period value[s,ms]   ... pwm period\n" \
        "-off value[%]         ... enforced off period of the led in percent\n" \
        "-max value[Mbps,kbps] ... maximum usb transfer rate\n" \
        "-min value[Mbps,kbps] ... minimum usb transfer rate\n" \
        "-pin value            ... pin to use\n" \
        "-inv                  ... invert the HIGH and LOW state\n"
    );
}

[[noreturn]] void unknown_argument_kill(string_view const &v) {
    cout << "Unknown or invalid argument: " << v << endl;
    print_help();
    exit(-1);
}

template<typename T, typename V = uint64_t>
T parse_value(string_view const &v, map<string_view, V> const &extentions) {
    int value = 0;
    auto [p, ec] = from_chars(v.data(), v.data() + v.size(), value);
    if (ec != errc{}) {
        unknown_argument_kill(v);
    }
    if (extentions.empty()) {
        return T(value);
    }
    string_view extention{ p };
    auto multiplier = extentions.find(extention);
    if (multiplier == extentions.end()) {
        unknown_argument_kill(extention);
    }
    return T(value * multiplier->second);
}

auto const size_extentions    = map<string_view, uint64_t> {{ "Mbps"sv, 1024*1024 }, { "kbps"sv, 1024 }};
auto const time_extentions    = map<string_view, uint64_t> {{ "s"sv, 1000 }, { "ms"sv, 1 }};
auto const percent_extentions = map<string_view, double>   {{ "%"sv, 1.0/100.0 }};

auto const zero_argument_commands = map<string_view, void(*)(Config &)> {
    { "-logging"sv, [](auto &cfg) { cfg.logging = true; }},
    { "-help"sv,    [](auto &cfg) { print_help();       }},
    { "-inv"sv,     [](auto &cfg) { cfg.invert = true;  }},
};

auto const one_argument_commands = map<string_view, void(*)(Config &, string_view)> {
    { "-period"sv, [](auto &cfg, auto value) { cfg.pwm_periode       = parse_value<duration_t>(value, time_extentions);    }},
    { "-max"sv,    [](auto &cfg, auto value) { cfg.max_transfer_rate = parse_value<uint64_t>  (value, size_extentions);    }},
    { "-min"sv,    [](auto &cfg, auto value) { cfg.min_transfer_rate = parse_value<uint64_t>  (value, size_extentions);    }},
    { "-pin"sv,    [](auto &cfg, auto value) { cfg.led_pin           = parse_value<int>       (value, {});                 }},
    { "-off"sv,    [](auto &cfg, auto value) { cfg.off_periode_ratio = parse_value<double>    (value, percent_extentions); }},
};

Config parse_arguments(arguments_t const &arguments) {
    Config cfg{};
    auto cur = arguments.cbegin();
    auto const end = arguments.cend();

    while (cur != end) {
        auto const argument = *cur++;
        if (auto cmd = zero_argument_commands.find(argument); cmd != zero_argument_commands.end())
            cmd->second(cfg);
        else if (auto cmd = one_argument_commands.find(argument); cmd != one_argument_commands.end() && cur != end)
            cmd->second(cfg, *cur++);
        else
            unknown_argument_kill(argument);
    }
    return cfg;   
}

int main(int argc, char *argv[]) {
    Config cfg = parse_arguments(arguments_t(argv + 1, argv + argc));
    cfg.print();
    cfg.calculate_periode_values();

    Raspi raspi{ cfg.led_pin, cfg.invert };
    UsbMon monitor{};

    generate_led_pwm(cfg, raspi, monitor);
    return 0;
}
