/**
 * @file QueryScratchCrashWorker.cpp
 * @brief Independent process termination and readonly/writable scratch recovery probe.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/LogicalQueryScratch.hpp"
#include <cstdlib>
#include <iostream>
using namespace graph;
using namespace graph::storage;
int main(int argc,char **argv){
	if(argc!=3 || (std::string(argv[1])!="crash" && std::string(argv[1])!="recover"))return 2;
	try {
		RelocatableStorageBackendOptions options;options.data_path=argv[2];options.query_scratch_enabled=true;
		if(std::string(argv[1])=="crash"){
			options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;auto backend=RelocatableStorageBackend::initializeNew(options);backend->open();
			const auto node=backend->createLogicalNodes({{{"CrashScratch"},{{"value",PropertyValue(7)}}}}).at(0);
			auto scratch=backend->openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);const std::vector<std::byte> bytes(4096,std::byte{0x3A});
			const auto handle=scratch->write(bytes);if(scratch->read(handle)!=bytes)return 3;
			std::cout<<node<<" "<<handle<<std::endl;std::_Exit(86);
		}
		const auto fileBytes=std::filesystem::file_size(options.data_path);
		options.open_mode=OpenMode::OPEN_READ_ONLY_EXISTING_FILE;RelocatableStorageBackend reader(options);reader.open();reader.resetIoStats();
		auto old=reader.openLogicalGraphReadSnapshot();const auto fence=*old->mutationFence();if(old->readNodeProperty(1,"value")!=PropertyValue(7))return 4;old.reset();
		bool privateScratch=false;try{static_cast<void>(reader.verifyIntegrity());}catch(const FormatException &){privateScratch=true;}
		if(!privateScratch||reader.ioStats().data.write_bytes!=0)return 5;reader.close();if(std::filesystem::file_size(options.data_path)!=fileBytes)return 9;
		options.open_mode=OpenMode::OPEN_EXISTING_FILE;RelocatableStorageBackend writer(options);writer.open();if(writer.currentNativeLogicalGraphMutationFence()!=fence)return 10;
		auto recovered=writer.openLogicalGraphReadSnapshot();if(recovered->readNodeProperty(1,"value")!=PropertyValue(7))return 6;recovered.reset();
		static_cast<void>(writer.verifyIntegrity());auto scratch=writer.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);
		const std::vector<std::byte> bytes(4096,std::byte{0x4B});auto handle=scratch->write(bytes);if(scratch->read(handle)!=bytes)return 7;
		scratch->finish();static_cast<void>(writer.verifyIntegrity());writer.close();std::cout<<"recovery passed"<<std::endl;return 0;
	}catch(const std::exception &error){std::cerr<<error.what()<<std::endl;return 8;}
}
