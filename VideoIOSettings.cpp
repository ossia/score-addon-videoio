#include "VideoIOSettings.hpp"

// =============================================================================
// Serialization
// =============================================================================

template <>
void DataStreamReader::read(const Gfx::VideoIO::VideoOutputSettings& n)
{
  m_stream << static_cast<int>(n.vendor) << n.deviceName << n.deviceIndex
           << n.channelIndex << n.width << n.height << n.rate << n.videoFormat
           << n.pixelFormat << n.useRDMA << n.mode8K << n.hdrMode;
  insertDelimiter();
}

template <>
void DataStreamWriter::write(Gfx::VideoIO::VideoOutputSettings& n)
{
  int vendor = 0;
  m_stream >> vendor >> n.deviceName >> n.deviceIndex >> n.channelIndex >> n.width
      >> n.height >> n.rate >> n.videoFormat >> n.pixelFormat >> n.useRDMA
      >> n.mode8K >> n.hdrMode;
  n.vendor = static_cast<Gfx::VideoIO::Vendor>(vendor);
  checkDelimiter();
}

template <>
void JSONReader::read(const Gfx::VideoIO::VideoOutputSettings& n)
{
  obj["Vendor"] = static_cast<int>(n.vendor);
  obj["DeviceName"] = n.deviceName;
  obj["DeviceIndex"] = n.deviceIndex;
  obj["ChannelIndex"] = n.channelIndex;
  obj["Width"] = n.width;
  obj["Height"] = n.height;
  obj["Rate"] = n.rate;
  obj["VideoFormat"] = n.videoFormat;
  obj["PixelFormat"] = n.pixelFormat;
  obj["UseRDMA"] = n.useRDMA;
  obj["Mode8K"] = n.mode8K;
  obj["HDRMode"] = n.hdrMode;
}

template <>
void JSONWriter::write(Gfx::VideoIO::VideoOutputSettings& n)
{
  n.vendor = static_cast<Gfx::VideoIO::Vendor>(obj["Vendor"].toInt());
  n.deviceName = obj["DeviceName"].toString();
  n.deviceIndex = obj["DeviceIndex"].toInt();
  n.channelIndex = obj["ChannelIndex"].toInt();
  n.width = obj["Width"].toInt();
  n.height = obj["Height"].toInt();
  n.rate = obj["Rate"].toDouble();
  n.videoFormat = obj["VideoFormat"].toString();
  n.pixelFormat = obj["PixelFormat"].toString();
  n.useRDMA = obj["UseRDMA"].toBool();
  n.mode8K = obj["Mode8K"].toInt();
  n.hdrMode = obj["HDRMode"].toInt();
}

SCORE_SERALIZE_DATASTREAM_DEFINE(Gfx::VideoIO::VideoOutputSettings);

template <>
void DataStreamReader::read(const Gfx::VideoIO::VideoInputSettings& n)
{
  m_stream << static_cast<int>(n.vendor) << n.deviceName << n.devicePath
           << n.deviceIndex << n.channelIndex << n.videoFormat << n.pixelFormat
           << n.resolutionMode << n.routingMode << n.useRDMA;
  insertDelimiter();
}

template <>
void DataStreamWriter::write(Gfx::VideoIO::VideoInputSettings& n)
{
  int vendor = 0;
  m_stream >> vendor >> n.deviceName >> n.devicePath >> n.deviceIndex
      >> n.channelIndex >> n.videoFormat >> n.pixelFormat >> n.resolutionMode
      >> n.routingMode >> n.useRDMA;
  n.vendor = static_cast<Gfx::VideoIO::Vendor>(vendor);
  checkDelimiter();
}

template <>
void JSONReader::read(const Gfx::VideoIO::VideoInputSettings& n)
{
  obj["Vendor"] = static_cast<int>(n.vendor);
  obj["DeviceName"] = n.deviceName;
  obj["DevicePath"] = n.devicePath;
  obj["DeviceIndex"] = n.deviceIndex;
  obj["ChannelIndex"] = n.channelIndex;
  obj["VideoFormat"] = n.videoFormat;
  obj["PixelFormat"] = n.pixelFormat;
  obj["ResolutionMode"] = n.resolutionMode;
  obj["RoutingMode"] = n.routingMode;
  obj["UseRDMA"] = n.useRDMA;
}

template <>
void JSONWriter::write(Gfx::VideoIO::VideoInputSettings& n)
{
  n.vendor = static_cast<Gfx::VideoIO::Vendor>(obj["Vendor"].toInt());
  n.deviceName = obj["DeviceName"].toString();
  n.devicePath = obj["DevicePath"].toString();
  n.deviceIndex = obj["DeviceIndex"].toInt();
  n.channelIndex = obj["ChannelIndex"].toInt();
  n.videoFormat = obj["VideoFormat"].toString();
  n.pixelFormat = obj["PixelFormat"].toString();
  n.resolutionMode = obj["ResolutionMode"].toInt();
  n.routingMode = obj["RoutingMode"].toInt();
  n.useRDMA = obj["UseRDMA"].toBool();
}

SCORE_SERALIZE_DATASTREAM_DEFINE(Gfx::VideoIO::VideoInputSettings);
