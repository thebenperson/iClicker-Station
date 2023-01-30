#include <array>
#include <complex>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>

#include <cstdint>
#include <cstring>

#include <errno.h>
#include <signal.h>

#include <fstream>

namespace IClicker {

	constexpr auto FREQUENCY   = 917e6;
	constexpr auto BANDWIDTH   = 445'666;
	constexpr auto BPS         = 152'381;

	constexpr auto SIZE_BUFFER = 1024;
	constexpr auto SIZE_SYNC   = 24;
	constexpr auto SIZE_PACKET = 8;

	SoapySDR::Device *device;
	SoapySDR::Stream *stream;

	std::complex<float> *buff;

	bool running = true;

	long long time;
	long long delta;

	std::complex<float> lastSample;

	void process(std::function<bool (std::complex<float>*)> proc) {

		int result;
		int dummy1;
		long long dummy2;

		void* buffs[] = {IClicker::buff};
		IClicker::lastSample = std::complex<float>(0, 0);
		IClicker::time = 0;

		while (IClicker::running) {

			result = IClicker::device->readStream(IClicker::stream, buffs, IClicker::SIZE_BUFFER, dummy1, dummy2, std::numeric_limits<long>::max());
			if (result == SOAPY_SDR_TIMEOUT) continue;

			for (auto i = 0; i < result; i++) {

				auto &sample = IClicker::buff[i];

				bool result = proc(&sample);
				if (!result) return;

				IClicker::lastSample = sample;
				IClicker::time += IClicker::delta;

			}

		}

	}

	void decodeID(std::uint8_t *out, std::uint8_t *in) {

		out[0] = (in[0] >> 3) | ((in[0] & 0x04) << 5) | ((in[1] & 0x01) << 6) | ((in[2] & 0x01) << 5);
		out[1] = (in[1] >> 1) | ((in[0] & 0x01) << 7) | (in[2] >> 7);
		out[2] = ((in[2] & 0x7C) << 1) | (in[3] >> 5);
		out[3] = out[0] ^ out[1] ^ out[2];

	}

}

int main(int argc, char* argv[]) {

	int result;

	struct ::sigaction action;
	action.sa_handler = [](int) {

		IClicker::running = false;
		std::cout << "Caught signal -- terminating" << std::endl;

	};

	action.sa_flags = 0;

	result = ::sigaction(SIGINT, &action, nullptr);
	if (result) {

		std::cerr << "Could not set signal handler for SIGINT -- " << std::strerror(errno) << std::endl;
		return 1;

	}

	result = ::sigaction(SIGTERM, &action, nullptr);
	if (result) {

		std::cerr << "Could not set signal handler for SIGTERM -- " << std::strerror(errno) << std::endl;
		return 1;

	}

	SoapySDR::registerLogHandler([](const ::SoapySDRLogLevel, const char*) {});

	try {

		if (argc == 2) IClicker::device = SoapySDR::Device::make(argv[1]);
		else IClicker::device = SoapySDR::Device::make();

	} catch (std::runtime_error &e) {

		std::cerr << "Could not open device -- " << e.what() << std::endl;
		return 1;

	}

	typedef SoapySDR::Device Device;
	std::unique_ptr<Device, void (*)(Device*)> devicePTR(IClicker::device, static_cast<void (*)(Device*)>(&Device::unmake));

	auto rates = IClicker::device->listSampleRates(SOAPY_SDR_RX, 0);
	auto rate = std::numeric_limits<double>::max();

	for (double i: rates) if (i > IClicker::BANDWIDTH && i < rate) rate = i;

	if (rate == std::numeric_limits<double>::max()) {

		std::cerr << "No suitable sample rates found" << std::endl;
		return 1;

	}

	IClicker::device->setSampleRate(SOAPY_SDR_RX, 0, rate);
	std::cout << "Set sample rate to " << rate << std::endl;

	IClicker::device->setFrequency(SOAPY_SDR_RX, 0, IClicker::FREQUENCY);
	std::cout << "Set frequency to " << IClicker::FREQUENCY << std::endl;

	IClicker::device->setBandwidth(SOAPY_SDR_RX, 0, IClicker::BANDWIDTH);
	std::cout << "Set bandwidth to " << IClicker::BANDWIDTH << std::endl;

	// todo: test for availability
	IClicker::device->setGainMode(SOAPY_SDR_RX, 0, true);

	//~ IClicker::device->setGain(SOAPY_SDR_RX, 0, 40);

	try {

		IClicker::stream = IClicker::device->setupStream(SOAPY_SDR_RX, "CF32");

	} catch (std::runtime_error &e) {

		std::cerr << "Could not create stream -- " << e.what() << std::endl;
		return 1;

	}

	typedef SoapySDR::Stream Stream;
	std::unique_ptr<Stream, void (*)(Stream*)> streamPTR(IClicker::stream, [](Stream *stream) {

		IClicker::device->closeStream(stream);

	});

	try {

		IClicker::device->activateStream(IClicker::stream);

	} catch (std::runtime_error &e) {

		std::cerr << "Could not activate stream -- " << e.what() << std::endl;
		return 1;

	}

	std::unique_ptr<Stream, void (*)(Stream*)> streamDeactivatePTR(IClicker::stream, [](Stream *stream) {

		IClicker::device->deactivateStream(stream);

	});

	IClicker::buff = new std::complex<float>[IClicker::SIZE_BUFFER];
	std::unique_ptr<std::complex<float>[]> buff(IClicker::buff);

	// format: {
	//
	//	address        (3 bytes),
	//	encoded ID     (3 bytes 1 nibble)
	//	button pressed (1 nibble)
	//	checksum       (1 byte)
	//
	// }

	std::array<std::uint8_t, IClicker::SIZE_PACKET> packet;
	IClicker::delta = 1e9 / rate;

	std::cout << std::hex << std::uppercase;

	std::ofstream out("out.csv");

	while (IClicker::running) {

		// wait for start

		IClicker::process([](std::complex<float> *in) -> auto {

			return (std::norm(*in) < 0.8);

		});

		auto syncMin = std::numeric_limits<double>::max();
		auto syncMax = std::numeric_limits<double>::min();

		// wait until after sync header

		//~ IClicker::process([&](std::complex<float> *in) -> auto {

			//~ if (IClicker::time >= IClicker::SIZE_SYNC * (1e9 / IClicker::BPS)) return false;
			//~ double freq = std::arg(*in / IClicker::lastSample);

			//~ if (freq < syncMin) syncMin = freq;
			//~ if (freq > syncMax) syncMax = freq;

			//~ return true;

		//~ });

		bool lastValue = false;
		std::size_t bit = 0;

		long long lastTime = 0;

		IClicker::process([&](std::complex<float> *in) -> auto {

			double freq = std::arg(*in / IClicker::lastSample);
			bool value = (freq - ((syncMax - syncMin) / 2)) > 0;

			out << std::norm(*in) << ',' << freq << ',' << std::endl;

			if (value != lastValue) {

				std::size_t n = (IClicker::time - lastTime) / (1e9 / IClicker::BPS);

				for (std::size_t i = 0; i < n; i++) {

					auto byte = bit / 8;
					if (byte >= IClicker::SIZE_PACKET) return false;

					packet[byte] |= lastValue << (7 - (bit % 8));
					bit++;

				}

				lastValue = value;
				lastTime  = IClicker::time;

			}

			return true;

		});

		auto sum = 0;

		for (auto i = 3; i < 7; i++)
			sum = (sum + packet[i]) % 256;

		if (sum != packet[7]) continue;

		std::uint8_t id[4];
		IClicker::decodeID(id, packet.begin() + 3);

		std::cout << "0x";
		for (auto i: id) std::cout << std::setw(2) << std::setfill('0') << (unsigned) i;

		std::cout << " selected ";
		unsigned char choice = packet[6] & 0x0F;

		switch (choice) {

			case 0x01: choice = 'A';
			break;

			case 0x05: choice = 'B';
			break;

			case 0x0D: choice = 'C';
			break;

			case 0x0E: choice = 'D';
			break;

			case 0x0A: choice = 'E';
			break;

		}

		std::cout << choice << std::endl;

	}

	return 0;

}
