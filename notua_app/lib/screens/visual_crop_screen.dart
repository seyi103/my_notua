import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:image/image.dart' as img;

import '../state/playlist_models.dart';

class VisualCropScreen extends StatefulWidget {
  const VisualCropScreen({super.key, required this.source, required this.edit});
  final Uint8List source;
  final PhotoEditParameters edit;

  @override
  State<VisualCropScreen> createState() => _VisualCropScreenState();
}

class _VisualCropScreenState extends State<VisualCropScreen> {
  late final img.Image image;
  late final Uint8List displayBytes;
  late Rect cropPixels;

  @override
  void initState() {
    super.initState();
    var decoded = img.bakeOrientation(img.decodeImage(widget.source)!);
    ImagePipeline.flattenOnWhite(decoded);
    for (var i = 0; i < widget.edit.quarterTurns % 4; i++) {
      decoded = img.copyRotate(decoded, angle: 90);
    }
    image = decoded;
    displayBytes = Uint8List.fromList(img.encodeJpg(image, quality: 90));
    cropPixels = _fromNormalized(widget.edit.crop) ?? _largestCrop();
  }

  Rect _largestCrop() {
    final width = (image.width ~/ 4) * 4;
    final heightFromWidth = width * 3 ~/ 4;
    if (heightFromWidth <= image.height) {
      return Rect.fromLTWH(0, (image.height - heightFromWidth) / 2, width.toDouble(), heightFromWidth.toDouble());
    }
    final height = (image.height ~/ 3) * 3;
    final cropWidth = height * 4 ~/ 3;
    return Rect.fromLTWH((image.width - cropWidth) / 2, 0, cropWidth.toDouble(), height.toDouble());
  }

  Rect? _fromNormalized(NormalizedCrop crop) {
    final rect = Rect.fromLTWH(crop.left * image.width, crop.top * image.height,
      crop.width * image.width, crop.height * image.height);
    final width = rect.width.round();
    final height = rect.height.round();
    if (width * 3 != height * 4) return null;
    return Rect.fromLTWH(rect.left.roundToDouble(), rect.top.roundToDouble(),
      width.toDouble(), height.toDouble());
  }

  void _move(Offset delta, Size displaySize) {
    final scale = displaySize.width / image.width;
    final moved = cropPixels.shift(delta / scale);
    setState(() => cropPixels = moved.shift(Offset(
      moved.left < 0 ? -moved.left : moved.right > image.width ? image.width - moved.right : 0,
      moved.top < 0 ? -moved.top : moved.bottom > image.height ? image.height - moved.bottom : 0,
    )));
  }

  void _resize(double delta, Size displaySize) {
    final scale = displaySize.width / image.width;
    final deltaUnits = (delta / scale / 4).round();
    final maxUnits = image.width ~/ 4;
    final units = (cropPixels.width ~/ 4 + deltaUnits).clamp(10, maxUnits);
    final width = (units * 4).toDouble();
    final height = width * 3 / 4;
    if (height > image.height) return;
    final center = cropPixels.center;
    final left = (center.dx - width / 2).clamp(0, image.width - width).toDouble();
    final top = (center.dy - height / 2).clamp(0, image.height - height).toDouble();
    setState(() => cropPixels = Rect.fromLTWH(left, top, width, height));
  }

  NormalizedCrop get result => NormalizedCrop(
    left: cropPixels.left.round() / image.width,
    top: cropPixels.top.round() / image.height,
    width: cropPixels.width.round() / image.width,
    height: cropPixels.height.round() / image.height,
  );

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('4:3 자르기'), actions: [
      TextButton(onPressed: () => Navigator.pop(context, result), child: const Text('완료')),
    ]),
    body: Center(child: LayoutBuilder(builder: (context, constraints) {
      final scale = (constraints.biggest.width / image.width) <
          (constraints.biggest.height / image.height)
        ? constraints.biggest.width / image.width
        : constraints.biggest.height / image.height;
      final displaySize = Size(image.width * scale, image.height * scale);
      final rect = Rect.fromLTWH(cropPixels.left * scale, cropPixels.top * scale,
        cropPixels.width * scale, cropPixels.height * scale);
      return SizedBox.fromSize(size: displaySize, child: Stack(children: [
        Positioned.fill(child: Image.memory(displayBytes, fit: BoxFit.fill)),
        Positioned.fromRect(rect: rect, child: GestureDetector(
          key: const ValueKey('visible-crop-rectangle'),
          onPanUpdate: (details) => _move(details.delta, displaySize),
          child: Container(decoration: BoxDecoration(border: Border.all(color: Colors.white, width: 3))),
        )),
        Positioned(left: rect.right - 28, top: rect.bottom - 28, child: GestureDetector(
          onPanUpdate: (details) => _resize(details.delta.dx, displaySize),
          child: const Icon(Icons.open_in_full, color: Colors.white, size: 28),
        )),
      ]));
    })),
  );
}
