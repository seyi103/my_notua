import 'dart:isolate';
import 'dart:typed_data';

import 'package:image/image.dart' as img;

import '../state/playlist_models.dart';

/// Firmware-compatible, headerless Spectra 6 Y8 frame processor.
class ImagePipeline {
  static const width = 1600;
  static const height = 1200;
  static const byteCount = width * height;
  static const y8Palette = <int>[0x00, 0xf8, 0x20, 0x40, 0x10, 0x30];
  static const rgbPalette = <(int, int, int)>[
    (0, 0, 0),
    (255, 255, 255),
    (255, 255, 0),
    (255, 0, 0),
    (0, 0, 255),
    (0, 255, 0),
  ];
  static const bayer8 = <List<int>>[
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
  ];

  static NormalizedCrop defaultCrop(Uint8List source, int quarterTurns) {
    var decoded = img.decodeImage(source);
    if (decoded == null) throw const ImageProcessingException('Invalid JPEG or PNG image.');
    decoded = img.bakeOrientation(decoded);
    final imageWidth = quarterTurns.isOdd ? decoded.height : decoded.width;
    final imageHeight = quarterTurns.isOdd ? decoded.width : decoded.height;
    final widthUnit = imageWidth ~/ 4;
    final heightUnit = imageHeight ~/ 3;
    final unit = widthUnit < heightUnit ? widthUnit : heightUnit;
    final width = unit * 4;
    final height = unit * 3;
    return NormalizedCrop(
      left: ((imageWidth - width) ~/ 2) / imageWidth,
      top: ((imageHeight - height) ~/ 2) / imageHeight,
      width: width / imageWidth,
      height: height / imageHeight,
    );
  }

  Future<ProcessedImage> process(
    Uint8List source,
    PhotoEditParameters edit, {
    bool fullSize = true,
  }) => Isolate.run(() => processSync(source, edit, fullSize: fullSize));

  Future<Uint8List> originalPreview(Uint8List source, PhotoEditParameters edit) =>
      Isolate.run(() => originalPreviewSync(source, edit));

  static Uint8List originalPreviewSync(Uint8List source, PhotoEditParameters edit) {
    var decoded = img.decodeImage(source);
    if (decoded == null) throw const ImageProcessingException('Invalid JPEG or PNG image.');
    decoded = img.bakeOrientation(decoded);
    for (var i = 0; i < edit.quarterTurns % 4; i++) {
      decoded = img.copyRotate(decoded, angle: 90);
    }
    final crop = edit.crop.clamped;
    final x = (crop.left * decoded.width).round();
    final y = (crop.top * decoded.height).round();
    final width = (crop.width * decoded.width).round();
    final height = (crop.height * decoded.height).round();
    final cropped = img.copyCrop(decoded, x: x, y: y, width: width, height: height);
    return Uint8List.fromList(img.encodePng(img.copyResize(cropped,
      width: 400, height: 300, interpolation: img.Interpolation.cubic)));
  }

  static ProcessedImage processSync(
    Uint8List source,
    PhotoEditParameters edit, {
    bool fullSize = true,
  }) {
    var decoded = img.decodeImage(source);
    if (decoded == null) throw const ImageProcessingException('Invalid JPEG or PNG image.');
    decoded = img.bakeOrientation(decoded);
    for (var i = 0; i < edit.quarterTurns % 4; i++) {
      decoded = img.copyRotate(decoded, angle: 90);
    }
    final crop = edit.crop.clamped;
    var x = (crop.left * decoded.width).round().clamp(0, decoded.width - 1).toInt();
    var y = (crop.top * decoded.height).round().clamp(0, decoded.height - 1).toInt();
    var cropWidth = (crop.width * decoded.width).round().clamp(1, decoded.width - x).toInt();
    var cropHeight = (crop.height * decoded.height).round().clamp(1, decoded.height - y).toInt();
    if (cropWidth < 4 || cropHeight < 3 || cropWidth * 3 != cropHeight * 4) {
      throw const ImageProcessingException('Crop must be an exact 4:3 selection.');
    }
    final cropped = img.copyCrop(decoded, x: x, y: y, width: cropWidth, height: cropHeight);
    final outWidth = fullSize ? width : 400;
    final outHeight = fullSize ? height : 300;
    final frame = img.copyResize(cropped, width: outWidth, height: outHeight,
        interpolation: img.Interpolation.cubic);
    final bytes = Uint8List(outWidth * outHeight);
    for (var py = 0; py < outHeight; py++) {
      for (var px = 0; px < outWidth; px++) {
        final pixel = frame.getPixel(px, py);
        var r = pixel.r.toDouble() + edit.brightness;
        var g = pixel.g.toDouble() + edit.brightness;
        var b = pixel.b.toDouble() + edit.brightness;
        r = (r - 128) * edit.contrast + 128;
        g = (g - 128) * edit.contrast + 128;
        b = (b - 128) * edit.contrast + 128;
        final lum = .299 * r + .587 * g + .114 * b;
        r = lum + (r - lum) * edit.saturation;
        g = lum + (g - lum) * edit.saturation;
        b = lum + (b - lum) * edit.saturation;
        // enhanceImage writes into Uint8ClampedArray before orderedDither.
        r = _clamp8(r).toDouble();
        g = _clamp8(g).toDouble();
        b = _clamp8(b).toDouble();
        final threshold = ((bayer8[py & 7][px & 7] + .5) / 64 - .5) * 110;
        final index = nearestPalette(
          _clamp8(r + threshold),
          _clamp8(g + threshold),
          _clamp8(b + threshold),
        );
        bytes[py * outWidth + px] = y8Palette[index];
        final color = rgbPalette[index];
        frame.setPixelRgb(px, py, color.$1, color.$2, color.$3);
      }
    }
    if (!validate(bytes, expectedLength: outWidth * outHeight)) {
      throw const ImageProcessingException('Generated frame violates the firmware Y8 contract.');
    }
    return ProcessedImage(
      width: outWidth,
      height: outHeight,
      y8: bytes,
      previewPng: Uint8List.fromList(img.encodePng(frame)),
    );
  }

  /// ECMAScript ToUint8Clamp, used by writes to ImageData's Uint8ClampedArray.
  static int _clamp8(double value) {
    if (value <= 0) return 0;
    if (value >= 255) return 255;
    final floor = value.floor();
    final fraction = value - floor;
    if (fraction < .5) return floor;
    if (fraction > .5) return floor + 1;
    return floor.isEven ? floor : floor + 1;
  }

  static int nearestPalette(int r, int g, int b) {
    var best = 0;
    var bestDistance = double.infinity;
    for (var i = 0; i < rgbPalette.length; i++) {
      final p = rgbPalette[i];
      final distance = (r - p.$1) * (r - p.$1) +
          (g - p.$2) * (g - p.$2) +
          (b - p.$3) * (b - p.$3);
      if (distance < bestDistance) {
        bestDistance = distance.toDouble();
        best = i;
      }
    }
    return best;
  }

  static bool validate(Uint8List bytes, {int expectedLength = byteCount}) =>
      bytes.length == expectedLength && bytes.every(y8Palette.contains);
}

class ProcessedImage {
  const ProcessedImage({required this.width, required this.height, required this.y8, required this.previewPng});
  final int width;
  final int height;
  final Uint8List y8;
  final Uint8List previewPng;
}

class ImageProcessingException implements Exception {
  const ImageProcessingException(this.message);
  final String message;
  @override
  String toString() => message;
}
