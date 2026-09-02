import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:notua_app/image_processing/photo_cache.dart';

void main() {
  test('atomically replaces outputs and cleans obsolete cache files', () async {
    final root = await Directory.systemTemp.createTemp('notua-cache-test');
    addTearDown(() => root.delete(recursive: true));
    final cache = TemporaryPhotoCache(root: root);
    final first = await cache.replaceOutputs('slide', Uint8List.fromList([1]),
      Uint8List.fromList([0x00]));
    await Future<void>.delayed(const Duration(microseconds: 2));
    final second = await cache.replaceOutputs('slide', Uint8List.fromList([2]),
      Uint8List.fromList([0xf8]), oldPreviewPath: first.previewPath,
      oldBinPath: first.binPath);
    expect(File(first.previewPath).existsSync(), isFalse);
    expect(File(first.binPath).existsSync(), isFalse);
    expect(File(second.previewPath).readAsBytesSync(), [2]);
    expect(File(second.binPath).readAsBytesSync(), [0xf8]);
    await cache.deletePaths([second.previewPath, second.binPath]);
    expect(root.listSync(), isEmpty);
  });
}
