#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "../cpp/DrawingSerialization.h"

using namespace nativedrawing;

namespace {

std::vector<uint8_t> validEmptyPayload() {
  std::vector<uint8_t> data;
  auto appendUint32 = [&](uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  };

  appendUint32(DrawingSerialization::CURRENT_VERSION);
  appendUint32(0);
  return data;
}

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    return false;
  }
  return true;
}

bool approxEqual(float lhs, float rhs, float epsilon = 0.001f) {
  return std::fabs(lhs - rhs) <= epsilon;
}

bool testDeserializeAcceptsEmptyValidPayload() {
  DrawingSerialization serializer;
  std::vector<Stroke> strokes;

  return expect(serializer.deserialize(validEmptyPayload(), strokes), "empty payload should deserialize") &&
         expect(strokes.empty(), "empty payload should produce zero strokes");
}

bool testSerializeRoundTripPreservesSingleStroke() {
  DrawingSerialization serializer;
  Stroke stroke;
  stroke.paint.setAntiAlias(true);
  stroke.paint.setStyle(SkPaint::kStroke_Style);
  stroke.paint.setStrokeWidth(3.5f);
  stroke.paint.setColor(0xff112233);
  stroke.paint.setAlpha(200);
  stroke.toolType = "pen";
  stroke.originalAlphaMod = 0.85f;

  Point point{};
  point.x = 42.0f;
  point.y = 84.0f;
  point.pressure = 1.0f;
  point.azimuthAngle = 0.0f;
  point.altitude = 1.57f;
  point.calculatedWidth = 3.5f;
  point.timestamp = 1234;
  stroke.points.push_back(point);

  std::vector<Stroke> original = {stroke};
  const std::vector<uint8_t> payload = serializer.serialize(original);
  std::vector<Stroke> restored;

  return expect(serializer.deserialize(payload, restored), "single stroke payload should deserialize") &&
         expect(restored.size() == 1, "round-trip should preserve stroke count") &&
         expect(restored[0].points.size() == 1, "round-trip should preserve point count") &&
         expect(approxEqual(restored[0].points[0].x, 42.0f), "round-trip should preserve x") &&
         expect(approxEqual(restored[0].points[0].y, 84.0f), "round-trip should preserve y") &&
         expect(approxEqual(restored[0].paint.getStrokeWidth(), 3.5f), "round-trip should preserve width") &&
         expect(restored[0].paint.getColor() == SkColorSetARGB(200, 0x11, 0x22, 0x33), "round-trip should preserve color") &&
         expect(restored[0].paint.getAlpha() == static_cast<U8CPU>(200), "round-trip should preserve alpha") &&
         expect(restored[0].toolType == "pen", "round-trip should preserve tool type");
}

bool testSerializeRoundTripPreservesRecognizedShapeGeometry() {
  DrawingSerialization serializer;
  Stroke stroke;
  stroke.paint.setAntiAlias(true);
  stroke.paint.setStyle(SkPaint::kStroke_Style);
  stroke.paint.setStrokeWidth(4.0f);
  stroke.paint.setColor(0xff112233);
  stroke.paint.setAlpha(255);
  stroke.toolType = "shape-rectangle";
  stroke.originalAlphaMod = 1.0f;

  const std::pair<float, float> corners[] = {
      {10.0f, 20.0f},
      {110.0f, 20.0f},
      {110.0f, 80.0f},
      {10.0f, 80.0f},
      {10.0f, 20.0f},
  };

  for (const auto& corner : corners) {
    Point point{};
    point.x = corner.first;
    point.y = corner.second;
    point.pressure = 1.0f;
    point.altitude = 1.57f;
    point.calculatedWidth = 4.0f;
    stroke.points.push_back(point);
  }

  std::vector<Stroke> restored;
  const std::vector<uint8_t> payload = serializer.serialize({stroke});

  return expect(serializer.deserialize(payload, restored), "shape payload should deserialize") &&
         expect(restored.size() == 1, "shape round-trip should preserve stroke count") &&
         expect(restored[0].toolType == "shape-rectangle", "shape round-trip should preserve shape tool type") &&
         expect(approxEqual(restored[0].path.getBounds().left(), 10.0f), "shape path should preserve left bound") &&
         expect(approxEqual(restored[0].path.getBounds().top(), 20.0f), "shape path should preserve top bound") &&
         expect(approxEqual(restored[0].path.getBounds().right(), 110.0f), "shape path should preserve right bound") &&
         expect(approxEqual(restored[0].path.getBounds().bottom(), 80.0f), "shape path should preserve bottom bound");
}

bool testDeserializeRejectsTruncatedPayload() {
  DrawingSerialization serializer;
  std::vector<Stroke> strokes;
  const std::vector<uint8_t> data = {0x04, 0x00, 0x00};
  return expect(!serializer.deserialize(data, strokes), "truncated payload should be rejected");
}

bool testDeserializeRejectsUnsupportedVersion() {
  DrawingSerialization serializer;
  std::vector<Stroke> strokes;
  std::vector<uint8_t> data;
  auto appendUint32 = [&](uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  };

  appendUint32(99);
  appendUint32(0);
  return expect(!serializer.deserialize(data, strokes), "unsupported version should be rejected");
}

bool testDeserializeRejectsImpossiblePointCount() {
  DrawingSerialization serializer;
  std::vector<Stroke> strokes;
  std::vector<uint8_t> data;
  auto appendUint32 = [&](uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  };
  auto appendByte = [&](uint8_t value) { data.push_back(value); };
  auto appendFloat = [&](float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(float));
    appendUint32(bits);
  };

  appendUint32(DrawingSerialization::CURRENT_VERSION);
  appendUint32(1);
  appendByte(0);
  appendUint32(0xff000000);
  appendFloat(2.5f);
  appendByte(255);
  appendUint32(999999999);

  return expect(!serializer.deserialize(data, strokes), "impossible point count should be rejected");
}

bool testMixedValidAndInvalidPayloadsNeverCrash() {
  DrawingSerialization serializer;

  for (int iteration = 0; iteration < 50; iteration += 1) {
    std::vector<Stroke> restored;
    if (!expect(serializer.deserialize(validEmptyPayload(), restored), "valid payload should keep deserializing")) {
      return false;
    }
    if (!expect(restored.empty(), "valid empty payload should stay empty")) {
      return false;
    }

    restored.clear();
    const std::vector<uint8_t> invalidPayload = {0x04, 0x00, 0x00};
    if (!expect(!serializer.deserialize(invalidPayload, restored), "invalid payload should be rejected every time")) {
      return false;
    }
  }

  return true;
}

} // namespace

int main() {
  const bool passed =
      testDeserializeAcceptsEmptyValidPayload() &&
      testSerializeRoundTripPreservesSingleStroke() &&
      testSerializeRoundTripPreservesRecognizedShapeGeometry() &&
      testDeserializeRejectsTruncatedPayload() &&
      testDeserializeRejectsUnsupportedVersion() &&
      testDeserializeRejectsImpossiblePointCount() &&
      testMixedValidAndInvalidPayloadsNeverCrash();

  if (!passed) {
    return 1;
  }

  std::cout << "DrawingSerialization smoke tests passed" << std::endl;
  return 0;
}
