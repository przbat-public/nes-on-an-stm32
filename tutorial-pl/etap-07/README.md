# Etap 07: czym jest kartridż

Wszystko, co dotąd napisaliśmy, było programem: jednym plikiem, który sam robi wszystko.
Gra to kartridż, czyli plastikowa kaseta z pamięcią, którą wkłada się do konsoli. W środku
siedzi jeden plik. Obok programu leży w nim grafika, a na początku garść bajtów, czyli
najmniejszych kawałków pamięci, które opisują całą resztę. Ten etap czyta taki plik i wypisuje,
co w nim jest.

## Trzy części jednego pliku

Plik z grą ma trzy części i leżą jedna za drugą. Najpierw **nagłówek**, czyli 16 bajtów
mówiących, co jest dalej. Potem **pamięć programu** z poleceniami dla procesora. Na końcu
**pamięć grafiki** ze wzorami, z których układ obrazu składa obraz; do tego układu zajrzymy
w etapie 08.

Kolejność nie jest przypadkowa. Zanim konsola wykona choćby jedno polecenie, musi wiedzieć,
ile bajtów ma pamięć programu, ile pamięć grafiki i pod jakie adresy je włożyć. Dlatego
nagłówek leży przed programem, a program z tego etapu czyta cały nagłówek i zagląda do jego
pierwszych ośmiu bajtów.

## Bank, czyli kawałek pamięci

Rozmiaru nie zapisujemy w bajtach, tylko w **bankach**. Bank to umówiony kawałek pamięci:
jeden bank programu ma 16 kilobajtów, jeden bank grafiki 8 kilobajtów. Kilobajt to 1024 bajty,
więc bank programu to 16 384 bajty. Po co tak? Bo pamięć dzieli się na równe kawałki
i podmienia je jednym ruchem.

Kartridż z ośmioma bankami programu nie pokazuje procesorowi wszystkich naraz. Procesor widzi
32 kilobajty pamięci programu, czyli dwa banki, a resztę kartridż podmienia w trakcie gry.
Wrócimy do tego w etapie 13.

## Szesnastkowo, czyli jak czyta się nagłówek

Bajt ma osiem bitów, a zapis dziesiętny nie pokazuje w nich niczego. Dlatego w plikach używa
się zapisu **szesnastkowego**, gdzie cyframi są `0`–`9` oraz `A`–`F`: `A` znaczy dziesięć,
a `F` piętnaście. Jedna cyfra to cztery bity, więc dwie cyfry to dokładnie jeden bajt i dlatego
nagłówek wypisujemy parami cyfr. W kodzie robi to `printf("%02X", value)`: `X` znaczy
„szesnastkowo", a `02` znaczy „zawsze dwie cyfry, nawet gdy pierwsza jest zerem". Pierwszy bajt
każdego kartridża to `4E`, co dziesiętnie znaczy 78, a jako znak `N`. Cztery pierwsze bajty
to `4E 45 53 1A`: litery `N`, `E`, `S` i znacznik końca pliku z dawnych systemów. Program,
który ich nie znajdzie, wie, że to nie kartridż.

Dalej lecą liczby: **bajt 5** mówi, ile banków ma pamięć programu, **bajt 6**, ile banków ma
pamięć grafiki, **bajt 7** zbiera znaczniki, a **bajt 8** to numer układu na płytce kartridża.
Numer układu to osiem bitów. Format rozkłada go jednak na dwa bajty: cztery bity w bajcie 7
i cztery w bajcie 8. W obu bajtach zostały wolne miejsca, więc tam trafiły, a program składa
je z powrotem w jedną liczbę.

## Jak to wygląda w kodzie

W `main.c` nie ma ani jednego rejestru sprzętowego, bo ten program nie chodzi na płytce, tylko
na komputerze. Nagłówek ląduje w tablicy 16 bajtów, a potem pola wyciągamy z niej po numerze:

```c
header->prg_banks = raw[4];      /* bajt 5: banki pamięci programu */
header->mapper = (header->flags7 & MAPPER_HIGH_NIBBLE) |
                 ((header->flags6 & MAPPER_HIGH_NIBBLE) >> MAPPER_LOW_SHIFT);
```

Tablica liczy od zera, tak jak w etapie 01, więc `raw[4]` to piąty bajt pliku. Strzałka `->`
sięga po pole struktury przez wskaźnik: do funkcji trafia adres struktury, a nie jej kopia.

Znak `&` to iloczyn bitowy: zostawia tylko te bity, które są zapalone w obu liczbach. Znak `|`
to suma bitowa: skleja dwie liczby w jedną. Znak `>>` przesuwa bity w prawo. Stała
`MAPPER_HIGH_NIBBLE` to `0xF0`, czyli cztery górne bity bajtu. Stała `MAPPER_LOW_SHIFT` to 4:
o tyle miejsc trzeba przesunąć te cztery bity, żeby trafiły na swoje miejsce. Te trzy zabiegi
to cała lektura nagłówka.

Skąd program bierze te bajty? Otwiera plik funkcją `fopen`, wyciąga z niego 16 bajtów funkcją
`fread` i zamyka go funkcją `fclose`. Nazwę pliku dostaje od systemu. Parametry `argc` i `argv`
w `main` to liczba argumentów z wiersza poleceń i ich lista: `argv[0]` to nazwa programu,
a `argv[1]` to ścieżka, którą wpisałeś. Tryb `"rb"` w `fopen` znaczy „czytaj bajty, nie tekst".
Bez tego system podmieniłby niektóre bajty i program z kartridża przestałby się zgadzać.
Na końcu program otwiera plik jeszcze raz, przewija go na koniec funkcją `fseek` i czyta
pozycję funkcją `ftell`, żeby zmierzyć długość.

## Plik testowy i uruchomienie

Prawdziwej gry w repozytorium nie ma, więc kartridż do ćwiczeń tworzy narzędzie emulatora.
Uruchom je z katalogu głównego repozytorium, czyli dwa poziomy nad `etap-07`:

```bash
python3 tools/make_test_rom.py
```

Narzędzie zapisuje 24 592 bajty do pliku `build/test.nes`; katalog `build` powstaje tam, gdzie
uruchomisz narzędzie. W środku jest nagłówek, 16 kilobajtów programu i 8 kilobajtów grafiki:
czcionka i kilka kształtów, narysowanych w samym skrypcie. `python3` to interpreter Pythona,
języka, w którym ten skrypt napisano. Komenda `python3 --version` powie ci, czy masz go
w systemie.

Ten etap jest programem na komputer, nie na płytkę, więc buduje go zwykły kompilator, a nie
ten do procesorów Arm, i nie ma tu czego wgrywać. Z katalogu `etap-07`:

```bash
make
./etap07 ../../build/test.nes
```

Bez `make` to samo załatwia jedna komenda: `cc -Wall -Wextra -o etap07 main.c`. Flagi
`-Wall -Wextra` każą kompilatorowi wypisać wszystkie podejrzane miejsca i program kompiluje
się bez ani jednego ostrzeżenia — to cała jego weryfikacja, bo nie ma tu sprzętu, więc nie ma
czego sprawdzać okiem. To samo sprawdza `make check` z katalogu `etap-07`. Natomiast
`make STAGE=07 check` z katalogu `tutorial-pl` nie zadziała. Tamto `make` buduje każdy etap
kompilatorem dla procesorów Arm, a ten program potrzebuje biblioteki komputera, na którym go
uruchamiasz.

## Co powinieneś zobaczyć

Program mówi po angielsku, tak jak kod: `program` to pamięć programu, `graphics` pamięć
grafiki, a `mapper` numer układu na kartridżu. `saves` mówi, czy kartridż pamięta stan gry
po wyłączeniu zasilania. `trainer` to dodatkowe 512 bajtów, które niektóre kartridże mają
przed programem. `mirroring` opisuje pamięć obrazu w konsoli: czy jej dwie połowy leżą jedna
nad drugą, czy obok siebie. Do tej pamięci zajrzymy w etapie 08.

```
../../build/test.nes
first 16 bytes: 4E 45 53 1A 01 01 01 00 00 00 00 00 00 00 00 00
  program:     16384 bytes  (16 KB)
  graphics:     8192 bytes  (8 KB)
  mapper:      0
  mirroring:  stacked (vertical)
  saves:       no
  trainer:     no
  file is 24592 bytes, header plus both memories is 24592
  the two memories account for the whole file
```

Czytaj to razem z listą bajtów. Piąty bajt to `01`, więc pamięć programu to jeden bank, czyli
16 kilobajtów. Szósty to też `01`, czyli jedna grafika, 8 kilobajtów. Siódmy bajt, też `01`,
ustawia te dwie połowy jedna nad drugą. Ósmy bajt to `00`, więc numer układu to 0: cała pamięć
kartridża jest widoczna od razu, bez podmieniania banków.

Na końcu program idzie jeszcze na koniec pliku i sprawdza, czy 16 + 16 384 + 8192 daje jego
długość.

## Ćwiczenia

Zmień `print_size`, żeby obok rozmiaru wypisywał liczbę banków. Ta funkcja dostanie o jeden
parametr więcej.

Zbuduj dwa pozostałe kartridże z repozytorium i puść na nie program. Z katalogu głównego
repozytorium `python3 tools/make_test_rom.py --mmc1`, a potem to samo z `--mmc3`. Pliki
wylądują w `build/mmc1.nes` i `build/mmc3.nes`. Z katalogu `etap-07` uruchom program tak samo
jak poprzednio, tylko ze ścieżką do innego pliku: `./etap07 ../../build/mmc1.nes`. Przełączniki
wybierają kartridże z układem, który podmienia banki w trakcie gry; same układy poznasz
w etapach 13 i 14. Oba pliki mają 64 kilobajty pamięci programu i 16 kilobajtów grafiki, ale
inny numer układu: 1 dla `mmc1.nes` i 4 dla `mmc3.nes`. Sprawdź, czy program wypisuje właśnie
te liczby.

Dodaj wypisanie liczby bajtów przypadających na jeden bank grafiki: podziel rozmiar całej
grafiki przez liczbę banków. Ma wyjść 8 kilobajtów.

Zmień w nagłówku testowego pliku ostatni bajt z `00` na `FF` i sprawdź, czy program nadal
uznaje plik za poprawny. Pracuj na kopii, żeby nie psuć oryginału. Z katalogu głównego
repozytorium:

```bash
cp build/test.nes build/test-zmieniony.nes
python3 -c "p='build/test-zmieniony.nes'; d=bytearray(open(p,'rb').read()); d[15]=0xFF; open(p,'wb').write(d)"
```

Ta komenda wczytuje plik, podmienia szesnasty bajt i zapisuje go z powrotem. Z katalogu
`etap-07` uruchom program na kopii: `./etap07 ../../build/test-zmieniony.nes`. Program nadal
ją przyjmie, bo sprawdza tylko cztery pierwsze bajty i zgodność długości, a resztę nagłówka
tylko wypisuje. A co się stanie, jeśli podmienisz pierwszy bajt?

## Co dalej

W etapie 08 zajrzymy do pamięci grafiki: obraz konsoli powstaje tam z kafelków, czyli małych
wzorów 8 na 8 pikseli.
