import 'dart:typed_data';

import 'package:image_picker/image_picker.dart';
import 'package:image/image.dart' as img;

abstract interface class PhotoPickerService {
  Future<SelectedPhoto?> pickPhoto();
  Future<SelectedPhoto?> retrieveLostPhoto();
}

class SystemPhotoPickerService implements PhotoPickerService {
  SystemPhotoPickerService({ImagePicker? picker}) : _picker = picker ?? ImagePicker();
  final ImagePicker _picker;
  @override
  Future<SelectedPhoto?> pickPhoto() async {
    final file = await _picker.pickImage(source: ImageSource.gallery, requestFullMetadata: false);
    return file == null ? null : _validate(file);
  }

  @override
  Future<SelectedPhoto?> retrieveLostPhoto() async {
    final response = await _picker.retrieveLostData();
    if (response.isEmpty) {
      return null;
    }
    if (response.exception != null) {
      throw response.exception!;
    }
    final files = response.files;
    final file = files != null && files.isNotEmpty ? files.first : response.file;
    return file == null ? null : _validate(file);
  }

  Future<SelectedPhoto> _validate(XFile file) async {
    final bytes = await file.readAsBytes();
    try {
      final decoder = img.findDecoderForData(bytes);
      if (decoder == null) {
        throw const FormatException(
          'Only valid JPEG and PNG photos are supported.',
        );
      }
      final extension = switch (decoder) {
        img.JpegDecoder() => 'jpg',
        img.PngDecoder() => 'png',
        _ => throw const FormatException(
          'Only valid JPEG and PNG photos are supported.',
        ),
      };
      if (decoder.decode(bytes) == null) {
        throw const FormatException('The selected JPEG or PNG is corrupt.');
      }
      return SelectedPhoto(
        name: file.name,
        bytes: bytes,
        extension: extension,
      );
    } on FormatException {
      rethrow;
    } on Object {
      throw const FormatException('The selected JPEG or PNG is corrupt.');
    }
  }
}

class SelectedPhoto {
  const SelectedPhoto({required this.name, required this.bytes, required this.extension});
  final String name;
  final Uint8List bytes;
  final String extension;
}
