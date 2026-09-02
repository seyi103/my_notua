import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:notua_app/image_processing/image_pipeline.dart';
import 'package:notua_app/screens/visual_crop_screen.dart';
import 'package:notua_app/state/playlist_models.dart';

import 'image_pipeline_test.dart' show fixture;

void main() {
  testWidgets('visible locked crop is the exact rectangle consumed by pipeline', (tester) async {
    final source = fixture(400, 600);
    NormalizedCrop? selected;
    await tester.pumpWidget(MaterialApp(home: Builder(builder: (context) => Scaffold(
      body: FilledButton(onPressed: () async {
        selected = await Navigator.push<NormalizedCrop>(context,
          MaterialPageRoute(builder: (_) => VisualCropScreen(source: source,
            edit: PhotoEditParameters(crop: ImagePipeline.defaultCrop(source, 0)))));
      }, child: const Text('crop')),
    ))));
    await tester.tap(find.text('crop'));
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('visible-crop-rectangle')), findsOneWidget);
    await tester.drag(find.byKey(const ValueKey('visible-crop-rectangle')), const Offset(0, 30));
    await tester.tap(find.text('완료'));
    await tester.pumpAndSettle();
    expect(selected, isNotNull);
    final processed = ImagePipeline.processSync(source,
      PhotoEditParameters(crop: selected!), fullSize: false);
    expect((processed.width, processed.height), (400, 300));
  });

  test('portrait crop coordinates reset correctly after rotation', () {
    final source = fixture(400, 600);
    final portrait = ImagePipeline.defaultCrop(source, 0);
    final rotated = ImagePipeline.defaultCrop(source, 1);
    expect(portrait, isNot(rotated));
    expect(() => ImagePipeline.processSync(source,
      PhotoEditParameters(quarterTurns: 1, crop: rotated), fullSize: false), returnsNormally);
  });
}
