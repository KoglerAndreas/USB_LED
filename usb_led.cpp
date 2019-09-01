#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <iostream>
#include <stdint.h>
#include <chrono>
#include <tuple>

#ifdef RAPI
#include <wiringPi.h>
#endif

using namespace std::chrono_literals;

using byte_per_s_t = uint64_t;
using seconds_t    = std::chrono::duration<double, std::ratio<1,1>>;

constexpr byte_per_s_t operator""_bps(long double value) {
    return static_cast<byte_per_s_t>(value);
}

constexpr byte_per_s_t operator""_kbps(long double value) {
    return static_cast<byte_per_s_t>(value * 1024);
}

constexpr byte_per_s_t operator""_Mbps(long double value) {
    return static_cast<byte_per_s_t>(value * 1024 * 1024);
}

//
// CONFIGURATION
//
constexpr int                       led_pin           = 1;
constexpr byte_per_s_t              max_transfer_rate = 10.0_Mbps; // maximum transfer rate for the bus
constexpr std::chrono::milliseconds pwm_periode       = 2000ms;
 
constexpr byte_per_s_t periode_max_transfer_rate = 
    max_transfer_rate * 
    std::chrono::duration_cast<seconds_t>(pwm_periode).count();

static_assert(periode_max_transfer_rate != 0, "");

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



// calculate the high and low duration of the led based on the settings
static std::pair<seconds_t, seconds_t> calculate_durations(uint64_t bytes) {
    double ratio = std::min((bytes * 100) / periode_max_transfer_rate, 100lu) / 100.0;
    return {pwm_periode * ratio, pwm_periode * (1.0 - ratio)};
}

// get the transfered byte from the last packet
static uint64_t get_transfered_bytes(int usbmon_fd) {
    usbmon_packet p;
    ssize_t n = read(usbmon_fd, &p, sizeof(usbmon_packet));
    if (n != 48) return 0;
    if (p.type != 'C') return 0;
    return p.length;
}


enum class LedState { On, Off};
static void set_led_state(LedState state) {
    // TODO diese library hier einbinden
#ifdef RAPI
    if (state == LedState::On) {
        digitalWrite(led_pin, HIGH);
    } else {
        digitalWrite(led_pin, LOW);
    }
#endif
}

// sum all the bytes over a given duration
static uint64_t accumulate_bytes_for(int usbmon_fd, seconds_t const &dur) {
    using namespace std::chrono;

    uint64_t byte_sum = 0;
    auto     start    = high_resolution_clock::now();
    while ( duration_cast<seconds_t>(high_resolution_clock::now() - start) < dur ) {
        byte_sum += get_transfered_bytes(usbmon_fd);
    }
    return byte_sum;
}

// automatically generate pwm time based on the sample interval and the maximum transfer rate 
static void generate_led_pwm() {
    int usbmon_fd = open("/dev/usbmon0", O_RDONLY);
    if (usbmon_fd == -1) {
        std::cerr << "Cannot open usbmon file, forgot sudo?\n";
        return;
    }

    for (uint64_t byte_acc = 0;;) {
        auto [duration_high, duration_low] = calculate_durations(byte_acc);
        byte_acc = 0;
        set_led_state(LedState::On);
        byte_acc += accumulate_bytes_for(usbmon_fd, duration_high);
        set_led_state(LedState::Off);
        byte_acc += accumulate_bytes_for(usbmon_fd, duration_low);
    }
}


int main() {
#ifdef RAPI
    wiringPiSetup();
    pinMode(led_pin, OUTPUT);
#endif
    generate_led_pwm();
    return 0;
}
