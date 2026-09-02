import 'dart:typed_data';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:image/image.dart' as img;
import 'package:image_picker/image_picker.dart';
import 'package:notua_app/image_processing/photo_picker_service.dart';

class FakeImagePicker extends ImagePicker {
  FakeImagePicker({this.picked, LostDataResponse? lost})
    : lost = lost ?? LostDataResponse.empty();
  final XFile? picked;
  final LostDataResponse lost;

  @override
  Future<XFile?> pickImage({required ImageSource source, double? maxWidth,
    double? maxHeight, int? imageQuality, CameraDevice preferredCameraDevice =
    CameraDevice.rear, bool requestFullMetadata = true}) async => picked;

  @override
  Future<LostDataResponse> retrieveLostData() async => lost;
}

Uint8List encoded(String format) {
  final image = img.Image(width: 4, height: 3);
  image.setPixelRgb(0, 0, 10, 20, 30);
  return Uint8List.fromList(format == 'jpg' ? img.encodeJpg(image) : img.encodePng(image));
}

void main() {
  test('validates bytes and does not require a filename extension', () async {
    final service = SystemPhotoPickerService(picker: FakeImagePicker(
      picked: XFile.fromData(encoded('png'), name: 'content-provider-file')));
    final photo = await service.pickPhoto();
    expect(photo!.extension, 'png');
  });

  test('rejects renamed GIF, WebP, and corrupt files', () async {
    final image = img.Image(width: 4, height: 3);
    final invalid = <Uint8List>[
      Uint8List.fromList(img.encodeGif(image)),
      base64Decode('UklGRiIAAABXRUJQVlA4IBYAAAAwAQCdASoBAAEADsD+JaQAA3AAAA=='),
      Uint8List.fromList([0xff, 0xd8, 0xff]),
    ];
    for (final bytes in invalid) {
      final service = SystemPhotoPickerService(picker: FakeImagePicker(
        picked: XFile.fromData(bytes, name: 'renamed.jpg')));
      await expectLater(service.pickPhoto(), throwsA(isA<FormatException>()));
    }
  });

  test('recovers and validates Android lost data through the same path', () async {
    final file = XFile.fromData(encoded('jpg'), name: 'lost-without-extension');
    final service = SystemPhotoPickerService(picker: FakeImagePicker(
      lost: LostDataResponse(file: file, files: [file], type: RetrieveType.image)));
    final recovered = await service.retrieveLostPhoto();
    expect(recovered!.extension, 'jpg');
  });
}
