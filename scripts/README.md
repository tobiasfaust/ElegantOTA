das elop.cpp bauen
<pre>
python .\scripts\generate_hex.py
</pre>
test the projekt
<pre>
C:\Users\tobia\.platformio\penv\Scripts\platformio.exe ci --lib="." --project-option="lib_ignore=AsyncTCP_RP2040W" --board=esp32dev examples/AsyncDemo/AsyncDemo.ino
</pre>