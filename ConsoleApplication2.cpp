#include <iostream>
#include <mutex>
#include <fstream>
#include <hidapi/hidapi.h>
#include <cppcodec/base64_rfc4648.hpp>
#include "constants.h"

static std::mutex g_mutex;
static hid_device* g_device = nullptr;
static std::chrono::steady_clock::time_point g_lastReset = std::chrono::steady_clock::now();

static inline uint8_t hi_u16(uint16_t v) { return static_cast<uint8_t>((v >> 8) & 0xff); }
static inline uint8_t lo_u16(uint16_t v) { return static_cast<uint8_t>(v & 0xff); }

static void ensure_hid_initialized() {
    static bool init = false;
    if (!init) {
        if (hid_init() != 0) {
            throw std::runtime_error("hid_init failure");
        }
        init = true;
    }
}

static void open_device() {
    ensure_hid_initialized();
    if (g_device) return;
    g_device = hid_open(constants::VENDOR_ID, constants::PRODUCT_ID, nullptr);
    if (!g_device) {
        throw std::runtime_error("hid_open failed");
    }
}

static void close_device() {
    if (g_device) {
        hid_close(g_device);
		g_device = nullptr;
    }
}

static bool send_image(hid_device* device, const std::vector<uint8_t>& image, bool fix) {
    if (!device) return false;
    uint8_t packets_sent = 0;
	std::vector<uint8_t> last_image;
    bool retval = false;
    constexpr size_t CHUNK_SIZE = 1016;

    for (size_t offset = 0; offset < image.size(); offset += CHUNK_SIZE) {
		size_t length = std::min(CHUNK_SIZE, image.size() - offset);
		const uint8_t* chunk = image.data() + offset;

        std::vector<uint8_t> imgdata;
		imgdata.reserve(8 + CHUNK_SIZE);

        uint8_t signature = 0x00;

        if (length < CHUNK_SIZE) {
            signature = 0x01;
			uint16_t chunk_length = static_cast<uint16_t>(length);
			uint8_t chunk_transmitted = static_cast<uint8_t>((chunk_length >> 8) & 0xFF);
			uint8_t chunk_remaining = static_cast<uint8_t>(chunk_length & 0xFF); //chunkand

            uint8_t header[8] = {
                constants::IMG_TX,
                0x05,
                0x40,
                signature,
                packets_sent,
                0x00,
                lo_u16(chunk_remaining),
                lo_u16(chunk_remaining),
            };

			imgdata.insert(imgdata.end(), header, header + 8);
			imgdata.insert(imgdata.end(), chunk, chunk + length);

            if (last_image.size() >= length) { //different in v1, using v2
                imgdata.insert(imgdata.end(), last_image.begin() + static_cast<std::ptrdiff_t>(length), last_image.end());
            }
        }
        else {
            signature = 0x00;
            uint8_t header[8] = {
                0x02,
                0x05,
                0x40,
                signature,
                packets_sent,
                0x00,
                0xF8,
                0x03
			};

			imgdata.insert(imgdata.end(), header, header + 8);
			imgdata.insert(imgdata.end(), chunk, chunk + length);
			last_image.assign(chunk, chunk + length);
        }

        int res = hid_write(device, reinterpret_cast<const unsigned char*>(imgdata.data()), static_cast<size_t>(imgdata.size()));
		retval = (res >= 0);

        if(signature == 0x01 && fix) {  //unfuck should be here as well
            uint16_t chunk_length = static_cast<uint16_t>(length);
            uint16_t chunk_transmitted_temp = static_cast<uint8_t>((chunk_length >> 8) & 0xff);
			uint16_t chunk_remaining_temp = static_cast<uint8_t>(chunk_length & 0xff);
            std::vector<uint8_t> fix_packet = {
                constants::CONTROL_REQUEST,
                constants::DEVICE_ALIVE,
                0x40,
                signature,
                packets_sent,
                0x00,
				lo_u16(chunk_remaining_temp),
				lo_u16(chunk_transmitted_temp),
            };

            (void)hid_send_feature_report(device, reinterpret_cast<const unsigned char*>(fix_packet.data()), fix_packet.size());

            std::cout << "unfuck condition met";
		}
        packets_sent++;
    }
	return retval;
}


//#if defined(TESTING)
static void do_open_device() {
    std::lock_guard<std::mutex> lock(g_mutex);
    open_device();

    uint8_t pkt1[8] = { constants::CONTROL_REQUEST, constants::DEVICE_STAT, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (hid_send_feature_report(g_device, pkt1, sizeof(pkt1)) < 0)
        throw std::runtime_error("Failed to say hello!");

    uint8_t pkt2[8] = { constants::CONTROL_REQUEST, constants::DEVICE_ALIVE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (hid_send_feature_report(g_device, pkt2, sizeof(pkt2)) < 0)
        throw std::runtime_error("Failed to issue request 0x19!");

    uint8_t pkt3[8] = { constants::CONTROL_REQUEST, 0x20, 0x00, 0x19, 0x79, 0xE7, 0x32, 0x2E };
    if (hid_send_feature_report(g_device, pkt3, sizeof(pkt3)) < 0)
        throw std::runtime_error("Failed to issue request 0x20!");

    uint8_t pkt4[8] = { constants::CONTROL_REQUEST, constants::SET_INTERFACE, 0x40, 0x01, 0x79, 0xE7, 0x32, 0x2E };
    if (hid_send_feature_report(g_device, pkt4, sizeof(pkt4)) < 0)
        throw std::runtime_error("Failed to set interface!");

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
	std::cout << "Device opened successfully." << std::endl;
}

static void do_close_device() {
    std::lock_guard<std::mutex> lock(g_mutex);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    open_device();

    uint8_t pkt[8] = { constants::CONTROL_REQUEST, 0x1E, 0x40, 0x01, 0x43, 0x00, 0x69, 0x00 };
    if (hid_send_feature_report(g_device, pkt, sizeof(pkt)) < 0)
        throw std::runtime_error("Failed to close LCD!");
}

static std::string read_text_file_trimmed(const std::string& path) {
    std::ifstream f(path, std::ios::in);
    if (!f) throw std::runtime_error("Failed to open file: " + path);

    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto is_ws = [](unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static void do_send_frame_b64_string(const std::string& base64) {
    std::lock_guard<std::mutex> lock(g_mutex);
    open_device();

    bool fix = false;
    auto now = std::chrono::steady_clock::now();
    if (now - g_lastReset > std::chrono::seconds(25)) {
        fix = true;
        g_lastReset = now;
    }

    std::vector<uint8_t> image = cppcodec::base64_rfc4648::decode<std::vector<uint8_t>>(base64);
    bool ok = send_image(g_device, image, fix);

    if (!ok) {
        std::this_thread::sleep_for(std::chrono::milliseconds(7000));
        close_device();
        open_device();
    }
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage:\n"
                << "  " << argv[0] << " open\n"
                << "  " << argv[0] << " close\n"
                << "  " << argv[0] << " send <frame.b64>\n";
            return 2;
        }

        std::string cmd = argv[1];

        if (cmd == "open") {
            do_open_device();
            std::cout << "open_device: ok\n";
        }
        else if (cmd == "close") {
            do_close_device();
            std::cout << "close_device: ok\n";
        }
        else if (cmd == "send") {
            if (argc < 3) {
                throw std::runtime_error("send requires a path to a .b64 file containing the base64 frame");
            }
            std::string b64 = read_text_file_trimmed(argv[2]);
            do_send_frame_b64_string(b64);
            std::cout << "send_frame: done\n";
        }
        else {
            throw std::runtime_error("Unknown command: " + cmd);
        }

        // cleanup
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            close_device();
        }
        hid_exit();
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}

//#endif


// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu