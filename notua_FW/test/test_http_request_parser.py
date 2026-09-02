"""Executable host tests for SoftAP HTTP request-line parsing."""
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path
ROOT=Path(__file__).parents[1]
class HttpRequestParserTest(unittest.TestCase):
 def test_origin_absolute_and_invalid_targets(self):
  source=textwrap.dedent(r'''
   #include <cassert>
   #include <cstring>
   #include "core/network/httpRequestParser.h"
   using namespace notua::http;
   UploadRequestResult parse(const char* line, uint8_t& slot) {
     UploadRequest request{}; const auto result=parseUploadRequestLine(line,strlen(line),5,request); slot=request.slot; return result;
   }
   int main() {
     uint8_t slot=0xff;
     assert(parse("PUT /images/3 HTTP/1.1",slot)==UploadRequestResult::ok && slot==3);
     assert(parse("PUT http://192.168.4.1/images/3 HTTP/1.1",slot)==UploadRequestResult::ok && slot==3);
     assert(parse("PUT http://192.168.4.1:80/images/0 HTTP/1.0",slot)==UploadRequestResult::ok && slot==0);
     assert(parse("PUT /other/3 HTTP/1.1",slot)==UploadRequestResult::invalidPath);
     assert(parse("PUT /images/ HTTP/1.1",slot)==UploadRequestResult::invalidPath);
     assert(parse("PUT /images/3/extra HTTP/1.1",slot)==UploadRequestResult::invalidSlot);
     assert(parse("PUT /images/x HTTP/1.1",slot)==UploadRequestResult::invalidSlot);
     assert(parse("PUT /images/3x HTTP/1.1",slot)==UploadRequestResult::invalidSlot);
     assert(parse("PUT /images/5 HTTP/1.1",slot)==UploadRequestResult::invalidSlot);
     assert(parse("PUT /images/30 HTTP/1.1",slot)==UploadRequestResult::invalidSlot);
     assert(parse("POST /images/3 HTTP/1.1",slot)==UploadRequestResult::invalidMethod);
     assert(parse("PUT /images/3 HTTP/2",slot)==UploadRequestResult::unsupportedVersion);
     assert(parse("PUT /images/3",slot)==UploadRequestResult::malformedRequestLine);
     assert(parse("PUT /images/3 HTTP/1.1 extra",slot)==UploadRequestResult::malformedRequestLine);
     assert(strcmp(uploadRequestError(UploadRequestResult::invalidMethod),"invalid-method")==0);
     assert(strcmp(uploadRequestError(UploadRequestResult::invalidPath),"invalid-path")==0);
     assert(strcmp(uploadRequestError(UploadRequestResult::invalidSlot),"invalid-slot")==0);
   }
  ''')
  with tempfile.TemporaryDirectory() as directory:
   binary=Path(directory)/'http_parser_test'
   subprocess.run(['g++','-std=c++17','-DNOTUA_SOFTAP_HTTP_SPIKE=1','-I',str(ROOT/'src'),'-x','c++','-',str(ROOT/'src/core/network/httpRequestParser.cpp'),'-o',str(binary)],input=source,text=True,check=True)
   subprocess.run([binary],check=True)
if __name__=='__main__': unittest.main()
