// Gibt QR-Matrizen als Text aus; tests/verify_qr.py dekodiert sie mit OpenCV.
#include "qr.h"
#include <cstdio>
#include <iostream>
int main() { std::string line; while (std::getline(std::cin, line)) { qr::Code c; if (!qr::Encode(line, c)) { printf("FEHLER\n"); continue; } printf("%d\n", c.size); for (int y = 0; y < c.size; y++) { for (int x = 0; x < c.size; x++) putchar(c.get(x, y) ? '1' : '0'); putchar('\n'); } } }
