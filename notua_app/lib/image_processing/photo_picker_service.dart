import 'dart:typed_data';

import 'package:image_picker/image_picker.dart';

abstract interface class PhotoPickerService {
  Future<SelectedPhoto?> pickPhoto();
}

class SystemPhotoPickerService implements PhotoPickerService {
  SystemPhotoPickerService({ImagePicker? picker}) : _picker = picker ?? ImagePicker();
  final ImagePicker _picker;
  @override
  Future<SelectedPhoto?> pickPhoto() async {
    final file = await _picker.pickImage(source: ImageSource.gallery, requestFullMetadata: false);
    if (file == null) return null;
    final lowerName = file.name.toLowerCase();
    if (!lowerName.endsWith('.jpg') &&
        !lowerName.endsWith('.jpeg') &&
        !lowerName.endsWith('.png')) {
      throw const FormatException('Only JPEG and PNG photos are supported.');
    }
    return SelectedPhoto(name: file.name, bytes: await file.readAsBytes());
  }
}

class SelectedPhoto {
  const SelectedPhoto({required this.name, required this.bytes});
  final String name;
  final Uint8List bytes;
}
