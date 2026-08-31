import 'package:flutter_test/flutter_test.dart';
import 'package:notua_app/main.dart';
void main(){testWidgets('shows transfer controls',(tester)async{await tester.pumpWidget(const MyApp());expect(find.text('Select Y8 BIN'),findsOneWidget);expect(find.text('Connect to Notua'),findsOneWidget);expect(find.text('Upload'),findsOneWidget);});}
