# Changelog

## [Unreleased]
- BP35A1 (SKSTACK IP テキストコマンド) 対応から BP35C1-J11-T01 (J11 UART IF バイナリコマンド) 対応へ全面置き換え。プロトコル層(libbp35/bp35cmd)を新規バイナリ層(j11)に置き換え、接続シーケンスを J11 UART IF 仕様に準拠。

## [v0.2.0] 2025-08-15
- Fix compile error on ESP-IDF 5.x environment

## [v0.1.1] 2025-03-03

- Properly handle restart timer

## [v0.1.0] 2024-12-30

- Initial release
