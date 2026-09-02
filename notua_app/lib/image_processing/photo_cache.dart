import 'dart:io';
import 'dart:typed_data';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

abstract interface class PhotoCache {
  Uint8List? bytesForPath(String path);
  Future<String> storeSource(String id, String extension, Uint8List bytes);
  Future<CachedOutputs> replaceOutputs(
    String id,
    Uint8List preview,
    Uint8List y8, {
    String? oldPreviewPath,
    String? oldBinPath,
  });
  Future<void> deletePaths(Iterable<String?> paths);
}

class TemporaryPhotoCache implements PhotoCache {
  TemporaryPhotoCache({Directory? root}) : _root = root;
  Directory? _root;

  @override
  Uint8List? bytesForPath(String path) => null;

  Future<Directory> get root async {
    final existing = _root;
    if (existing != null) {
      return existing..createSync(recursive: true);
    }
    final temporary = await getTemporaryDirectory();
    return _root = await Directory(p.join(temporary.path, 'notua_photo_cache'))
        .create(recursive: true);
  }

  @override
  Future<String> storeSource(String id, String extension, Uint8List bytes) async {
    final directory = await root;
    final path = p.join(directory.path, '$id-source.$extension');
    await _atomicWrite(path, bytes);
    return path;
  }

  @override
  Future<CachedOutputs> replaceOutputs(
    String id,
    Uint8List preview,
    Uint8List y8, {
    String? oldPreviewPath,
    String? oldBinPath,
  }) async {
    final directory = await root;
    final stamp = DateTime.now().microsecondsSinceEpoch;
    final previewPath = p.join(directory.path, '$id-$stamp.png');
    final binPath = p.join(directory.path, '$id-$stamp.bin');
    await _atomicWrite(previewPath, preview);
    try {
      await _atomicWrite(binPath, y8);
    } on Object {
      final previewFile = File(previewPath);
      if (await previewFile.exists()) {
        await previewFile.delete();
      }
      rethrow;
    }
    await deletePaths([oldPreviewPath, oldBinPath]);
    return CachedOutputs(previewPath: previewPath, binPath: binPath);
  }

  @override
  Future<void> deletePaths(Iterable<String?> paths) async {
    for (final path in paths.whereType<String>().toSet()) {
      final file = File(path);
      if (await file.exists()) {
        await file.delete();
      }
    }
  }

  Future<void> _atomicWrite(String path, Uint8List bytes) async {
    final temporary = File('$path.part');
    await temporary.writeAsBytes(bytes, flush: true);
    await temporary.rename(path);
  }
}

class CachedOutputs {
  const CachedOutputs({required this.previewPath, required this.binPath});
  final String previewPath;
  final String binPath;
}
