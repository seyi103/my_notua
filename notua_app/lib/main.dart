import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
void main()=>runApp(const MyApp());
class MyApp extends StatelessWidget { const MyApp({super.key}); @override Widget build(BuildContext context)=>MaterialApp(title:'Notua transfer spike',theme:ThemeData(colorScheme:ColorScheme.fromSeed(seedColor:Colors.indigo),useMaterial3:true),home:const TransferScreen()); }
class TransferScreen extends StatefulWidget { const TransferScreen({super.key}); @override State<TransferScreen> createState()=>_TransferScreenState(); }
class _TransferScreenState extends State<TransferScreen>{
 static const method=MethodChannel('notua/softap'), progress=EventChannel('notua/softap_progress'); String? path; String status='Select an existing 1,920,000-byte Y8 BIN (photo conversion is out of scope)'; double fraction=0,instant=0,average=0; int elapsed=0; StreamSubscription? sub;
 @override void initState(){super.initState();sub=progress.receiveBroadcastStream().listen((dynamic e){final m=Map<String,dynamic>.from(e);setState((){fraction=m['sent']/m['total'];instant=m['instantKiBs'];average=m['averageKiBs'];elapsed=m['elapsedMs'];});});}
 @override void dispose(){sub?.cancel();super.dispose();}
 Future<void> select()async{try{final p=await method.invokeMethod<String>('selectFile');setState((){path=p;status=p??'No file';});}catch(e){setState(()=>status='$e');}}
 Future<void> connect()async{try{final info=await method.invokeMethod<String>('connect');setState(()=>status='Notua AP requested: $info');}catch(e){setState(()=>status='$e');}}
 Future<void> upload()async{if(path==null)return;setState(()=>status='Uploading through Android Network…');try{final r=await method.invokeMethod('upload',{'path':path,'slot':0});setState(()=>status='HTTP / CRC result: $r');}catch(e){setState(()=>status='$e');}}
 @override Widget build(BuildContext context)=>Scaffold(appBar:AppBar(title:const Text('Notua SoftAP transfer spike')),body:Padding(padding:const EdgeInsets.all(24),child:Column(crossAxisAlignment:CrossAxisAlignment.stretch,children:[ElevatedButton(onPressed:select,child:const Text('Select Y8 BIN')),ElevatedButton(onPressed:connect,child:const Text('Connect to Notua')),ElevatedButton(onPressed:path==null?null:upload,child:const Text('Upload')),const SizedBox(height:24),LinearProgressIndicator(value:fraction),Text('${(fraction*100).toStringAsFixed(1)}%'),Text('Instant ${instant.toStringAsFixed(1)} KiB/s · Average ${average.toStringAsFixed(1)} KiB/s'),Text('Elapsed ${(elapsed/1000).toStringAsFixed(1)} s'),const SizedBox(height:20),SelectableText(status)])));
}
