#include "../include.h"
#include "../client/client.h"
#include "../util/util.h"
#include "process.h"
#include "mapper.h"

void mmap::thread(tcp::client& client) {
	while (client.state != tcp::client_state::imports_ready) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	util::system_data_t dat;
	util::fetch_system_data(dat);

	auto needle = std::find_if(dat.processes.begin(), dat.processes.end(), [&](util::process_data_t& dat) {
		return dat.name == client.selected_game.process_name;
	});

	if (needle == dat.processes.end()) {
		io::log_error("failed to find process.");
		return;
	}

	util::process32 proc(*needle);

	if (!proc.open()) {
		return;
	}

	if (!proc.enum_modules()) {
		io::log_error("failed to enum {} modules", proc.name());
		return;
	}

	auto image = proc.allocate(client.mapper_data.image_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!image) {
		io::log_error("failed to allocate memory for image.");
		return;
	}

	io::log("image base : {:x}", image);

	auto imports = nlohmann::json::parse(client.mapper_data.imports);

	nlohmann::json final_imports;
	for (auto& [key, value] : imports.items()) {
		for (auto& i : value) {
			auto name = i.get<std::string>();

			final_imports[name] = proc.module_export(proc.map(key), name);
		}
	}
	imports.clear();

	nlohmann::json resp;
	resp["alloc"] = image;
	resp["id"] = client.selected_game.id;

	client.write(tcp::packet_t(resp.dump(), tcp::packet_type::write, client.session_id, tcp::packet_id::image));
	resp.clear();

	client.stream(final_imports.dump());
	final_imports.clear();

	io::log("please wait...");
	while (client.state != tcp::client_state::image_ready) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	if (!proc.write(image, client.mapper_data.image.data(), client.mapper_data.image.size())) {
		io::log_error("failed to write image.");
		return;
	}
	client.mapper_data.image.clear();

	auto entry = image + client.mapper_data.entry;

	io::log("entry : {:x}", entry);

	static std::vector<uint8_t> shellcode = { 0x55, 0x89, 0xE5, 0x6A, 0x00, 0x6A, 0x01, 0x68, 0xEF, 0xBE,
		0xAD, 0xDE, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0xFF, 0xD0, 0x89, 0xEC, 0x5D, 0xC3 };

	*reinterpret_cast<uint32_t*>(&shellcode[8]) = image;
	*reinterpret_cast<uint32_t*>(&shellcode[13]) = entry;

	auto code = proc.allocate(shellcode.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!proc.write(code, shellcode.data(), shellcode.size())) {
		io::log_error("failed to write shellcode.");
		return;
	}

	io::log("shellcode : {:x}", code);

	proc.thread(code);

	proc.free(code, shellcode.size());

	//proc.free(image, client.mapper_data.image_size);

	proc.close();

	client.state = tcp::client_state::injected;

	io::log("done");
}