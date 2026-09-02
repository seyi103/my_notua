import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:image/image.dart' as img;
import 'package:notua_app/image_processing/image_pipeline.dart';
import 'package:notua_app/state/playlist_models.dart';

Uint8List fixture(int width, int height) {
  final image = img.Image(width: width, height: height);
  for (var y = 0; y < height; y++) {
    for (var x = 0; x < width; x++) {
      image.setPixelRgb(x, y, x * 255 ~/ width, y * 255 ~/ height, (x + y) * 127 ~/ (width + height));
    }
  }
  return Uint8List.fromList(img.encodePng(image));
}

Uint8List alphaFixture(int alpha) {
  final image = img.Image(width: 4, height: 3, numChannels: 4);
  for (var y = 0; y < image.height; y++) {
    for (var x = 0; x < image.width; x++) {
      image.setPixelRgba(x, y, 0, 0, 0, alpha);
    }
  }
  return Uint8List.fromList(img.encodePng(image));
}

Uint8List grayFixture(int value) {
  final image = img.Image(width: 4, height: 3);
  for (var y = 0; y < image.height; y++) {
    for (var x = 0; x < image.width; x++) {
      image.setPixelRgb(x, y, value, value, value);
    }
  }
  return Uint8List.fromList(img.encodePng(image));
}

int fnv1a(Uint8List bytes) {
  var hash = 0x811c9dc5;
  for (final byte in bytes) {
    hash = ((hash ^ byte) * 0x01000193) & 0xffffffff;
  }
  return hash;
}

void main() {
  test('ports the reference Bayer matrix and Spectra mapping exactly', () {
    expect(ImagePipeline.bayer8[0], [0, 32, 8, 40, 2, 34, 10, 42]);
    expect(ImagePipeline.bayer8[7], [63, 31, 55, 23, 61, 29, 53, 21]);
    expect(ImagePipeline.y8Palette, [0x00, 0xf8, 0x20, 0x40, 0x10, 0x30]);
    expect(ImagePipeline.nearestPalette(255, 0, 0), 3);
  });

  test('fractional Bayer boundary matches dither_reference.html', () {
    expect(ImagePipeline.nearestPalette(
      166.8828220917325,
      87.81140305633194,
      247.2703154572156,
    ), 4);
    expect(ImagePipeline.nearestPalette(167, 88, 247), 1,
      reason: 'Rounding before lookup reproduces the reviewed regression.');
  });

  test('transparent PNG pixels are flattened onto white', () {
    const edit = PhotoEditParameters(brightness: 0, contrast: 1, saturation: 1);
    final transparent = ImagePipeline.processSync(alphaFixture(0), edit,
      fullSize: false);
    expect(transparent.y8.toSet(), {0xf8});
    final partial = ImagePipeline.processSync(alphaFixture(127), edit,
      fullSize: false);
    final compositedGray = ImagePipeline.processSync(grayFixture(128), edit,
      fullSize: false);
    expect(partial.y8, compositedGray.y8);
  });

  test('generates exact landscape dimensions, bytes, and palette-only Y8', () {
    final result = ImagePipeline.processSync(fixture(8, 6), const PhotoEditParameters());
    expect((result.width, result.height), (1600, 1200));
    expect(result.y8, hasLength(1920000));
    expect(ImagePipeline.validate(result.y8), isTrue);
  });

  test('non-trivial gradient matches independently generated HTML checksum', () {
    final result = ImagePipeline.processSync(fixture(400, 300),
      const PhotoEditParameters(brightness: -18, contrast: .5, saturation: 1.65),
      fullSize: false);
    // This checksum covers adjustment order, Uint8 clamping, Bayer phase,
    // nearest-palette tie behavior and Y8 byte mapping from the HTML port.
    expect(fnv1a(result.y8), 0x37274d1d);
  });

  test('brightness contrast and saturation independently affect output', () {
    final source = fixture(12, 9);
    Uint8List run(PhotoEditParameters edit) => ImagePipeline.processSync(source, edit, fullSize: false).y8;
    final base = run(const PhotoEditParameters());
    expect(run(const PhotoEditParameters(brightness: 30)), isNot(base));
    expect(run(const PhotoEditParameters(contrast: 1.5)), isNot(base));
    expect(run(const PhotoEditParameters(saturation: 2)), isNot(base));
  });

  test('portrait and landscape rotation/crop remain 4:3', () {
    for (final source in [fixture(6, 8), fixture(8, 6)]) {
      final crop = ImagePipeline.defaultCrop(source, 1);
      final result = ImagePipeline.processSync(source,
        PhotoEditParameters(quarterTurns: 1, crop: crop), fullSize: false);
      expect((result.width, result.height), (400, 300));
    }
  });

  test('invalid images fail clearly', () {
    final invalid = Uint8List.fromList([1, 2, 3]);
    expect(
      () => ImagePipeline.defaultCrop(invalid, 0),
      throwsA(isA<ImageProcessingException>()),
    );
    expect(
      () => ImagePipeline.originalPreviewSync(
        invalid,
        const PhotoEditParameters(),
      ),
      throwsA(isA<ImageProcessingException>()),
    );
    expect(
      () => ImagePipeline.processSync(
        invalid,
        const PhotoEditParameters(),
      ),
      throwsA(isA<ImageProcessingException>()),
    );
  });
}
