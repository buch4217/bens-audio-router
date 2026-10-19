# Tests

Kern-Tests laufen nativ, ohne Windows:

```bash
cd tests
g++ -O3 -ffast-math -std=c++17 -I../src test_core.cpp  -o test_core  && ./test_core    # Resampler: Qualität, Aliasing, Dither, CPU
g++ -O2 -std=c++17 -I../src test_calib.cpp -o test_calib && ./test_calib               # Einmessung: Messgenauigkeit
g++ -O2 -std=c++17 -I../src test_flac.cpp  -o test_flac  && ./test_flac 24 48000 && flac -t -s /tmp/t.flac   # FLAC-Encoder (Referenz-Decoder "flac")
pip install cryptography && python3 verify_crypto.py                                   # SHA-1/RSA/AES gegen OpenSSL
g++ -O2 -std=c++17 -pthread -I../src test_wavrec.cpp -o test_wavrec && mkdir -p /tmp/wt && ./test_wavrec /tmp/wt && python3 verify_wavrec.py /tmp/wt   # WAV/FLAC-Rekorder (Aufnahme-Box): Formate, RF64, Header
g++ -O2 -std=c++17 -I../src test_route.cpp -o test_route && ./test_route          # Kanal-Matrix des Summen-Moduls (Mono -> L/R)
```

Sonos- und AirPlay-Simulation: `tools/mock_sonos.py` und `tools/raop_test_sender.py`.
