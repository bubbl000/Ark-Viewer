#pragma once
#include <string>
#include "ImageDecoder.h"
#include <vector>

// 解码器注册中心接口
void RegisterDecoder(DecoderFactoryFunc factory);
std::unique_ptr<ImageDecoder> FindDecoder(const uint8_t* magic, size_t len);
std::unique_ptr<ImageDecoder> FindDecoderByExtension(const std::string& ext);
std::vector<std::string> SupportedExtensions();

