#include <v4l2/V4L2Colorimetry.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace Gfx::V4L2;

TEST_CASE("uvcvideo YUYV with no descriptor is limited-range BT.601 sRGB", "[v4l2]")
{
  const auto c = colorimetryFromV4L2(
      V4L2_COLORSPACE_SRGB, V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT,
      V4L2_XFER_FUNC_DEFAULT, false);
  CHECK(c.space == AVCOL_SPC_SMPTE170M);
  CHECK(c.primaries == AVCOL_PRI_BT709);
  CHECK(c.trc == AVCOL_TRC_IEC61966_2_1);
  CHECK(c.range == AVCOL_RANGE_MPEG);
}

TEST_CASE("the JPEG colorspace is full-range BT.601", "[v4l2]")
{
  const auto c = colorimetryFromV4L2(
      V4L2_COLORSPACE_JPEG, V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT,
      V4L2_XFER_FUNC_DEFAULT, false);
  CHECK(c.space == AVCOL_SPC_SMPTE170M);
  CHECK(c.range == AVCOL_RANGE_JPEG);
}

TEST_CASE("an explicit quantization and encoding win over the defaults", "[v4l2]")
{
  const auto c = colorimetryFromV4L2(
      V4L2_COLORSPACE_SRGB, V4L2_YCBCR_ENC_709, V4L2_QUANTIZATION_FULL_RANGE,
      V4L2_XFER_FUNC_709, false);
  CHECK(c.space == AVCOL_SPC_BT709);
  CHECK(c.trc == AVCOL_TRC_BT709);
  CHECK(c.range == AVCOL_RANGE_JPEG);
}

TEST_CASE("Rec.709 HDMI capture defaults to limited-range BT.709", "[v4l2]")
{
  const auto c = colorimetryFromV4L2(
      V4L2_COLORSPACE_REC709, V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT,
      V4L2_XFER_FUNC_DEFAULT, false);
  CHECK(c.space == AVCOL_SPC_BT709);
  CHECK(c.primaries == AVCOL_PRI_BT709);
  CHECK(c.trc == AVCOL_TRC_BT709);
  CHECK(c.range == AVCOL_RANGE_MPEG);
}

TEST_CASE("BT.2020 PQ is kept", "[v4l2]")
{
  const auto c = colorimetryFromV4L2(
      V4L2_COLORSPACE_BT2020, V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT,
      V4L2_XFER_FUNC_SMPTE2084, false);
  CHECK(c.space == AVCOL_SPC_BT2020_NCL);
  CHECK(c.primaries == AVCOL_PRI_BT2020);
  CHECK(c.trc == AVCOL_TRC_SMPTE2084);
  CHECK(c.range == AVCOL_RANGE_MPEG);
}

TEST_CASE("RGB formats default to full range", "[v4l2]")
{
  const auto c = colorimetryFromV4L2(
      V4L2_COLORSPACE_SRGB, V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT,
      V4L2_XFER_FUNC_DEFAULT, true);
  CHECK(c.range == AVCOL_RANGE_JPEG);
}

TEST_CASE("raw sensor data is linear with no primaries", "[v4l2]")
{
  const auto c = colorimetryFromV4L2(
      V4L2_COLORSPACE_RAW, V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT,
      V4L2_XFER_FUNC_DEFAULT, true);
  CHECK(c.trc == AVCOL_TRC_LINEAR);
  CHECK(c.primaries == AVCOL_PRI_UNSPECIFIED);
  CHECK(c.range == AVCOL_RANGE_JPEG);
}

TEST_CASE("a driver that reports no colorspace says nothing", "[v4l2]")
{
  const auto c = colorimetryFromV4L2(
      V4L2_COLORSPACE_DEFAULT, V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT,
      V4L2_XFER_FUNC_DEFAULT, false);
  CHECK(c.space == AVCOL_SPC_UNSPECIFIED);
  CHECK(c.primaries == AVCOL_PRI_UNSPECIFIED);
  CHECK(c.trc == AVCOL_TRC_UNSPECIFIED);
  CHECK(c.range == AVCOL_RANGE_UNSPECIFIED);
}
