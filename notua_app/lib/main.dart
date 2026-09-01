import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import 'screens/home_screen.dart';
import 'screens/transfer_screen.dart';
import 'state/playlist_models.dart';
import 'sync/fake_sync_service.dart';

void main() => runApp(const NotuaApp());

class NotuaApp extends StatelessWidget {
  const NotuaApp({super.key, this.initialDraft, this.syncService});

  final PlaylistDraft? initialDraft;
  final SynchronizationService? syncService;

  @override
  Widget build(BuildContext context) {
    const ink = Color(0xff203f60);
    final routes = <String, WidgetBuilder>{};
    if (kDebugMode) routes['/developer/transfer'] = (_) => const TransferScreen();
    return MaterialApp(
      title: 'Notua',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        scaffoldBackgroundColor: const Color(0xfffaf8f3),
        colorScheme: ColorScheme.fromSeed(
          seedColor: ink,
          primary: ink,
          surface: const Color(0xfffffdf9),
        ),
        textTheme: const TextTheme(bodyMedium: TextStyle(color: Color(0xff282a2c))),
        sliderTheme: const SliderThemeData(trackHeight: 3),
      ),
      routes: routes,
      home: HomeScreen(
        draft: initialDraft ?? PlaylistDraft.withMockData(),
        syncService: syncService ?? FakeSynchronizationService(),
      ),
    );
  }
}
