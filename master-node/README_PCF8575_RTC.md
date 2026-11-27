# PCF8575 + PIR + pora 12:00 (XIAO nRF52840)

## Zmienione pliki

- `boards/xiao_ble.overlay`: I2C przeniesione na D6 (SDA=P1.11), D7 (SCL=P1.12),
  `int-gpios` z PCF8575 podlaczone do D0 (P0.02). PIR podlaczony do PCF P04
  (HIGH = ruch), wyjscie budzenia kamery P06. Testowe linie `trigger` i
  `battery-trigger` zostaly usuniete; realne wejscie to `pir-gpios`.
- `boards/xiao_ble.conf`: `CONFIG_APP_IMAGE_WAKE_PULSE_MS=30` tylko dla XIAO.
- `src/main.c`: inicjalizacja LED, obsluga PIR przez PCF8575,
  reakcja na impuls 80 ms o poludniu. Zostawiono nazwy istniejacych funkcji.
- `src/noon_clock.c`, `src/noon_clock.h`: harmonogram RTC przez `k_timer`.
- `CMakeLists.txt`, `Kconfig`: dolaczenie nowego pliku i opcje zegara.

**Nie zmieniono `src/camera_link.c`, `src/camera_link.h`, `src/image_store.c`,
`src/image_store.h`, `src/camera_node.c`, `src/camera_node.h`, `src/cam_wake.c`,
`src/cam_wake.h` ani konfiguracji Bluetooth w `prj.conf`.**

## Polaczenia

| XIAO | PCF8575 / inne |
|---|---|
| D6 (P1.11) | SDA |
| D7 (P1.12) | SCL |
| D0 (P0.02) | **INT** (WYJSCIE open drain z PCF, aktywne LOW) |
| 3V3 | VCC PCF8575 (jesli modul jest zgodny z 3.3 V) |
| GND | GND PCF8575 i czujnika PIR |
| - | `A0`, `A1`, `A2` do GND -> adres I2C `0x20` |
| Wyjscie PIR (3.3 V push-pull) | P04 (wejscie, aktywne HIGH) |
| Wejscie wybudzania kamery | P06 (wyjscie aktywne LOW) |

**Nie lacz PIR bezposrednio z D0.** PCF8575 nie kopiuje stanu PIR.
Zamiast tego `/INT` generuje LOW przy zmianie stanu dowolnego wejscia.
Daj zewnetrzny rezystor ok. **4.7 kOhm miedzy INT i 3.3 V**.
W overlay dla D0 wlaczono tez `GPIO_PULL_UP` jako pomocniczy.
Wymagane wyprowadzenia w `zephyr,user` to teraz `pir-gpios` i
`cam-wake-gpios`; nie ma juz testowych `trigger-gpios` ani
`battery-trigger-gpios`. Funkcja `read_battery()` w API kamery pozostaje
nietknieta, ale nie uruchamia jej juz przycisk testowy.
Daj rezystory pull-up SDA/SCL do 3.3 V (np. 4.7 kOhm) i sprawdz,
ze modul nie ma juz wlasnych.

O ile moze pracowac drugi element na tej samej magistrali I2C,
jesli ma inny adres i zgodne napiecia. I2C nie uzywa slave-select.

## Przebieg PIR

1. PIR robi HIGH na P04; PCF8575 sprowadza `/INT` do LOW na D0.
2. Sterownik Zephyr `gpio_pcf857x` (`int-gpios`) obsluguje IRQ D0, wykonuje
   I2C read, w system workqueue uruchamia dotychczasowy `trigger_isr` dla P04.
3. `main()` sprawdza aktualny poziom P04; reaguje **wylacznie na HIGH**.
4. LED (`led0`) swieci 2 sekundy, bez blokowania transmisji.
5. `take_image()` (BEZ zmian) uruchamia BLE i funkcja `cam_wake_pulse()`
   robi 30 ms LOW na **PCF P06**, po czym wraca do HIGH.
6. Caly mechanizm BLE, parowania, odebrania danych i przechowywania obrazu
   dziala jak poprzednio.

Uwaga: PCF8575 reaguje na obydwa zbocza. Przy bardzo krotkich impulsach
PIR zmiana moze zniknac, zanim PCF zostanie odczytany po I2C.
Sterownik PCF857x 4.4 moze rowniez skasowac INT przy zapisie do portu.
To rozwiazanie przeznaczone jest do PIR generujacego sygnal HIGH przez
stosunkowo dlugi czas, nie do impulsow mikrosekundowych.

## RTC: ograniczenie czasu rzeczywistego

nRF52840 ma niskoczestotliwosciowy licznik RTC, ale **nie ma
samodzielnego zegara kalendarzowego z aktualna godzina 12:00**.
`k_timer` jest napedzany przez systemowy zegar LF RTC i moze wybudzac
procesor z Zephyr System ON. Nie jest to alarm wybudzajacy z System OFF.

Dodalem implementacje, ktora po **synchronizacji znanej godziny lokalnej**
wyzwoli zdarzenie o nastepnej 12:00 i co 24 h potem.
Sam callback zegara nie wykonuje I2C: budzi `main()` przez semafor, gdzie
nastepuje `cam_wake_pulse(80)` na P06. Jesli `main()` wykonuje transfer BLE,
impuls 80 ms bedzie opozniony do zakonczenia tego transferu.

**Domyslnie nie jest uzbrojona, bo w dostarczonym projekcie nie ma zrodla
aktualnej godziny.** Implementacja nie udaje, ze czas od bootowania to
czas kalendarzowy. Zeby uzbroic alarm:

- Na potrzeby szybkiego testu mozna wpisac w `boards/xiao_ble.conf` np.:
  `CONFIG_APP_RTC_BOOT_HOUR=11`, `CONFIG_APP_RTC_BOOT_MINUTE=59`,
  `CONFIG_APP_RTC_BOOT_SECOND=50`, **jesli nRF uruchomisz naprawde o 11:59:50**.
  Ustawienia sa interpretowane jako godzina w chwili uruchomienia firmware.
- Docelowo: uzupelnij projekt o sposob ustawienia godziny przy starcie, np.
  zewnętrzny RTC na I2C lub ustawienie przez hosta (USB).
  W takim kodzie nalezy wywolac
  `noon_clock_set_local_time(godzina, minuta, sekunda)` (z `noon_clock.h`).

Przy zmianie czasu letni/zimowy i po restarcie trzeba ponownie synchronizowac.
Dla stalego czasu 12:00 kazdego dnia bez synchronizacji po wlaczeniu
potrzebny jest zegar czasu rzeczywistego z podtrzymaniem.

**Wazne**: aktualny projekt rozpoznaje `300 ms` jako polecenie raportu
baterii. Sam impuls `80 ms` jest krotszy od progu `200 ms` kamery i
moze zostac zinterpretowany przez tamto urzadzenie jako polecenie
wykonania zdjecia. Nie zmienilem kamery ani BLE, zgodnie z poleceniem.

## Kompilacja

```sh
cd /workdir/master-node
west build -b xiao_ble -d build -p always
```

Projekt posiada wlasna konfiguracje USB CDC ACM: **nie dodawaj**
`-S cdc-acm-console` bez sprawdzenia konfiguracji plytki.

**Nie mozna bylo zbudowac tego projektu w srodowisku generowania plikow**,
poniewaz nie zawiera ono SDK Zephyr ani toolchainu ARM. Sprawdz build
w Twoim Dev Containerze. Wszystkie pliki BLE zachowano bajt w bajt.
