import 'package:flutter/material.dart';

import '../state/playlist_models.dart';

class PhotoPlaceholder extends StatelessWidget {
  const PhotoPlaceholder({
    super.key,
    required this.color,
    this.parameters = const PhotoEditParameters(),
    this.radius = 20,
  });
  final int color;
  final PhotoEditParameters parameters;
  final double radius;

  @override
  Widget build(BuildContext context) {
    final brightness = parameters.brightness * 255 / 60;
    final contrast = parameters.contrast;
    final saturation = parameters.saturation;
    final turns = parameters.quarterTurns;
    final inverseSaturation = 1 - saturation;
    final red = .213 * inverseSaturation;
    final green = .715 * inverseSaturation;
    final blue = .072 * inverseSaturation;
    final translate = 128 * (1 - contrast) + brightness;
    return ClipRRect(
      borderRadius: BorderRadius.circular(radius),
      child: RotatedBox(
        quarterTurns: turns,
        child: ColorFiltered(
          colorFilter: ColorFilter.matrix(<double>[
            contrast * (red + saturation),
            contrast * green,
            contrast * blue,
            0,
            translate,
            contrast * red,
            contrast * (green + saturation),
            contrast * blue,
            0,
            translate,
            contrast * red,
            contrast * green,
            contrast * (blue + saturation),
            0,
            translate,
            0,
            0,
            0,
            1,
            0,
          ]),
          child: CustomPaint(
            painter: _LandscapePainter(Color(color)),
            child: const SizedBox.expand(),
          ),
        ),
      ),
    );
  }
}

class _LandscapePainter extends CustomPainter {
  const _LandscapePainter(this.base);
  final Color base;
  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(Offset.zero & size, Paint()..color = base);
    canvas.drawCircle(
      Offset(size.width * .77, size.height * .2),
      size.shortestSide * .1,
      Paint()..color = const Color(0xffffe7b1),
    );
    final back = Path()
      ..moveTo(0, size.height * .68)
      ..lineTo(size.width * .32, size.height * .3)
      ..lineTo(size.width * .58, size.height * .65)
      ..lineTo(size.width * .76, size.height * .4)
      ..lineTo(size.width, size.height * .7)
      ..lineTo(size.width, size.height)
      ..lineTo(0, size.height)
      ..close();
    canvas.drawPath(back, Paint()..color = const Color(0xff536a65));
    final front = Path()
      ..moveTo(0, size.height * .76)
      ..quadraticBezierTo(
        size.width * .4,
        size.height * .58,
        size.width,
        size.height * .82,
      )
      ..lineTo(size.width, size.height)
      ..lineTo(0, size.height)
      ..close();
    canvas.drawPath(front, Paint()..color = const Color(0xffd7c3a2));
  }

  @override
  bool shouldRepaint(_LandscapePainter oldDelegate) => oldDelegate.base != base;
}
