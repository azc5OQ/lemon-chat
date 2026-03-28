#!/usr/bin/python3
import base64
import os

#converts .wasm file to base64 string so it can be embedded in client.html

base_location = '/home/user/Desktop/lemonchat-dev/client-webassemblies/libopusjs/libopus-workingbuild/dist/'
file = open(base_location + 'libopus.wasm', 'rb')
file_content = file.read()
result = base64.b64encode(file_content).decode('ascii')
file.close()

file = open(base_location + 'libopus_wasm_base64.txt','w').close()
file = open(base_location + 'libopus_wasm_base64.txt','a')
file.write("var libopus_webassembly_base64 = '")
file.write(result)
file.write("';")
file.close()
print("check libopus_wasm_base64.txt")

