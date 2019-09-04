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

auto now() noexcept {
    return chrono::high_resolution_clock::now();
}

template<typename T>
double to_sec(T const &d) noexcept {
    return chrono::duration_cast<seconds_t>(d).count();
}

template<typename T>
T multiply_duration(T const &d, double ratio) noexcept {
    return T(static_cast<typename T::rep>(d.count() * ratio));
} 

// Configuration change defaults here
struct Config {
    bool       logging           = false;
    uint64_t   max_transfer_rate = 10 * 1024 * 1024;
    uint64_t   min_transfer_rate = 0;
    duration_t pwm_periode       = 100ms;
    double     off_periode_ratio = 0.1;
    int        led_gpio_pin      = 17;

    friend ostream& operator<<(ostream &s, Config const &cfg) {
        s << "\nConfiguration:\n";
        s << "-logging: "           << boolalpha << cfg.logging       << '\n';
        s << "-period: "            << to_sec(cfg.pwm_periode)        << " s\n";
        s << "-off_period_ratio: "  << cfg.off_periode_ratio * 100.0  << "%\n";
        s << "-max_transfer_rate: " << cfg.max_transfer_rate / 1024.0 << " kbps\n";
        s << "-min_transfer_rate: " << cfg.min_transfer_rate / 1024.0 << " kbps\n";
        s << "-gpio: "              << cfg.led_gpio_pin               << '\n';
        return s;
    }

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
    int      fd                = open("/dev/usbmon0", O_RDONLY | O_NONBLOCK);
    uint64_t accumulated_bytes = 0;
public:
    UsbMon() {
        if (fd == -1) {
            cerr << "Cannot open usbmon device! forgot sudo?\n";
            exit(-1);
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
        return chrono::duration_cast<duration_t>(now() - tsc);
    }
    uint64_t get_and_reset_accumulated_bytes() noexcept {
        return exchange(accumulated_bytes, 0);
    }
private:
    // get the transfered byte from the last packet
    uint64_t get_transfered_bytes() const noexcept {
        constexpr auto type_offset   = 8;
        constexpr auto length_offset = 32;
        unsigned char buffer[64];
        // lagacy read only returns 48 bytes
        if (read(fd, &buffer, 64) != 48) return 0; 
        // only accumulate the CALLBACK type
        if (buffer[type_offset] != 'S' && buffer[type_offset] != 'C') return 0; 
        return *reinterpret_cast<unsigned int*>(buffer[length_offset]);
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
    puts(
        "-help                 ... print this message\n" \
        "-logging              ... enable logging\n" \
        "-period value[s,ms]   ... pwm period\n" \
        "-off value[%]         ... enforced off period of the led in percent\n" \
        "-max value[Mbps,kbps] ... maximum usb transfer rate\n" \
        "-min value[Mbps,kbps] ... minimum usb transfer rate\n" \
        "-gpio value           ... GPIO pin to use\n"
    );
}

[[noreturn]] void unknown_argument_kill(string_view const &v) {
    cout << "Unknown or invalid argument: " << v << endl;
    print_help();
    exit(-1);
}

template<typename T = uint64_t>
T parse_value(string_view const &v, map<string_view, uint64_t> const &extentions) {
    int value = 0;
    auto [p, ec] = from_chars(v.data(), v.data() + v.size(), value);
    if (ec != errc{}) {
        unknown_argument_kill(v);
    }
    if (extentions.empty()) {
        return T(value);
    }
    string_view extention{ p };
    auto function = extentions.find(extention);
    if (function == extentions.end()) {
        unknown_argument_kill(extention);
    }
    return T(value * function->second);
}

auto const size_extentions = map<string_view, uint64_t>{
    {"Mbps"sv, 1024 * 1024 },
    {"kbps"sv, 1024        }
};
auto const time_extentions = map<string_view, uint64_t>{
    {"s"sv,  1000 },
    {"ms"sv, 1 }
};
auto const percent_extentions = map<string_view, uint64_t>{
    { "%"sv, 1 }
};
auto const zero_argument_commands = map<string_view, void(*)(Config &)> {
    { "-logging"sv, [](auto &cfg) { 
        cfg.logging = true;
    }},
    { "-help"sv, [](auto &cfg) { 
        print_help();
    }},
};
auto const one_argument_commands = map<string_view, void(*)(Config &, string_view)> {
    { "-period"sv, [](auto &cfg, auto value) { 
        cfg.pwm_periode = parse_value<duration_t>(value, time_extentions); 
    }},
    { "-max"sv, [](auto &cfg, auto value) { 
        cfg.max_transfer_rate = parse_value(value, size_extentions);
    }},
    { "-min"sv, [](auto &cfg, auto value) { 
        cfg.min_transfer_rate = parse_value(value, size_extentions);
    }},
    { "-gpio"sv, [](auto &cfg, auto value) { 
        cfg.led_gpio_pin = parse_value<int>(value, {});
    }},
    { "-off"sv, [](auto &cfg, auto value) { 
        auto percent = parse_value(value, percent_extentions);
        if (percent < 0 || percent > 100) {
            unknown_argument_kill(value);
        }
        cfg.off_periode_ratio = percent / 100.0;
    }},
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
    cout << cfg << endl;
    cfg.calculate_periode_values();

    UsbMon monitor{};
    
#ifdef USING_WIRING_PI
    wiringPiSetupGpio();
    pinMode(cfg.led_gpio_pin, OUTPUT);
#endif
    generate_led_pwm(cfg, monitor);
    return 0;
}
