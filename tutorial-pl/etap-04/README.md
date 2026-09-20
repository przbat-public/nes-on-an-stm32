# Etap 04: jak obraz trafia na panel, czyli pięć przewodów i pasma

W etapie 00 kolor leciał wprost na panel. Nie było pośrednika, więc nie było czego
przerysować, z czym porównać ani na czym zaoszczędzić. Ten etap pokazuje drogę, którą obraz
pokonuje do panelu: pięć przewodów, po których lecą bajty, i pasma, na które dzielimy obraz.
Zegar i bufor obrazu zostają takie, jakie ustawił etap 03.

Do kodu panelu z tego etapu wracają wszystkie następne etapy na płytkę, dlatego warto
przeczytać go uważnie.

## Pięć przewodów, czyli czym rozmawiamy z panelem

Panel nie jest pamięcią, do której sięgasz adresem. Bajty wysyła się do niego po przewodach,
a pięć wystarcza, bo każdy ma jedną robotę:

- **SCK** (od angielskiego *serial clock*) to takt: jedno uderzenie na jeden bit,
- **MOSI** (*master out, slave in*) to dane od płytki do panelu,
- **CS** (*chip select*) mówi panelowi „teraz mówię do ciebie",
- **DC** (*data/command*) mówi, czy ten bajt to rozkaz, czy dane,
- **RST** to reset panelu.

Panel nic nie odsyła, więc wszystkie pięć przewodów idzie w jedną stronę. Taki sposób
wysyłania bajtów, po jednym bicie na uderzenie taktu, nazywa się **SPI** (od angielskiego
*Serial Peripheral Interface*). W kodzie rozpoznasz przewody po nazwach `PIN_SCK`,
`PIN_MOSI`, `PIN_CS`, `PIN_DC` i `PIN_RST`, a jeden bajt wysyła funkcja `spi_byte`: wkłada
go do rejestru układu SPI i czeka, aż wyjdzie drutem.

## Zegar zostaje na 80 MHz

Zegar podnieśliśmy w etapie 03 i ten etap niczego w nim nie zmienia. Wracam do niego tylko
po to, żeby było jasne, skąd bierze się 40 MHz na drucie do panelu: szyna dostaje połowę
taktu rdzenia.

Arytmetykę warto przeliczyć jeszcze raz, bo od niej zależy cała reszta. Źródłem jest
wewnętrzny oscylator HSI16, czyli 16 MHz. Za nim stoją mnożnik i dzielnik; w dokumentacji
noszą nazwy N i R. Mnożnik ustawiamy na 10, dzielnik na 2: szesnaście razy dziesięć daje
160 MHz w środku układu, a połowa z tego to 80 MHz dla rdzenia. Trzy liczby to trzy pola
jednego rejestru, więc zmiana któregokolwiek z nich zmienia takt całego układu, nie tylko
szyny do panelu.

Przy 80 MHz sama pamięć flash nie nadąża za rdzeniem, dlatego przed włączeniem PLL
ustawiamy cztery stany oczekiwania i włączamy pamięci podręczne. Kolejność tych kroków
opisuje rozdział etapu 03; tutaj nie ma nic nowego.

## Bufor obrazu, czyli dlaczego jeden bajt na piksel

Panel przyjmuje kolory w formacie RGB565: pięć bitów na czerwony, sześć na zielony i pięć
na niebieski, razem dwa bajty na piksel. Gdyby bufor trzymał takie kolory, sam obraz
z konsoli, 256 na 240 pikseli, zajmowałby 122 880 bajtów, czyli 120 kilobajtów. Cały panel
ma 320 na 240 pikseli, więc jego obraz w kolorach byłby jeszcze większy: 150 kilobajtów.
Mikrokontroler ma 96 kilobajtów pamięci, więc żaden z tych dwóch obrazów by się w niej
nie zmieścił.

Dlatego bufor trzyma **indeksy do palety**, tak jak w etapach 02 i 03: jeden bajt na piksel,
a kolor powstaje dopiero w chwili wysyłania. Ten sam obraz zajmuje wtedy 61 440 bajtów, czyli
60 kilobajtów, a paleta to osiem wpisów po dwa bajty; w emulatorze będzie ich 64, czyli
128 bajtów. Zamiana indeksu na kolor kosztuje jedno odwołanie do tablicy na piksel i to jest
cała cena tego rozwiązania.

Bufor obejmuje tylko obraz z konsoli. Panel ma 320 pikseli szerokości, więc po bokach
zostają dwa czarne pasy po 32 piksele. Malujemy je raz przy starcie i nigdy więcej ich
nie dotykamy.

## Pasma, czyli po co dzielić obraz

Cały obraz to 122 880 bajtów. Przy 40 MHz na drucie zajmuje to 24,6 milisekundy, bo każdy
bajt to równo osiem taktów szyny. To dużo, jeśli porównać z czasem, jaki emulator potrzebuje
na policzenie jednej klatki.

Dlatego obraz nie leci jednym kawałkiem. Dzielimy go na pasma po osiem linii, czyli
po 4096 bajtów, i wysyłamy pasmo po paśmie. Pasmo to najmniejszy kawałek obrazu, jaki da
się wysłać osobno. Dzięki temu program nie musi czekać na koniec całej klatki, żeby coś
policzyć, a panel zaczyna pokazywać obraz, zanim dotrze reszta. W tym etapie nie ma jeszcze
czego liczyć między pasmami, więc pasma są pokazówką mechanizmu. W etapie 08 złożymy z nich
pierwszy prawdziwy obraz z kartridża, a w etapie 10 każde pasmo dostanie własne przerwanie.

## Wzór testowy

Ekran wypełnia osiem pionowych pasów kolorów, a w lewym górnym narożniku siedzi biały
kwadrat. Ta asymetria jest celowa, tak samo jak zielony kwadrat w etapie 03: jednolity kolor
ukryłby obrót panelu o 180 stopni. Teraz nie ukryje. Jeśli biały kwadrat pojawi się w innym
narożniku, niż powinien, wiesz, że orientacja jest ustawiona inaczej.

## Zbuduj i wgraj

```bash
# z katalogu tutorial-pl:
make STAGE=04 flash
```

To samo ręcznie, jeśli chcesz zobaczyć każdy krok:

```bash
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 -Wall -Wextra -nostartfiles \
    -T ../src/linker.ld ../src/startup_l476.s etap-04/main.c -o etap04.elf
arm-none-eabi-objcopy -O binary etap04.elf etap04.bin
st-flash write etap04.bin 0x08000000
st-flash reset
```

## Co powinieneś zobaczyć

Osiem pionowych pasów i biały kwadrat w lewym górnym narożniku, a po bokach czarne pasy
szerokości 32 pikseli. Obraz stoi: ten etap rysuje go raz i zostawia na ekranie. Ruch wróci
w etapie 10, gdy program zacznie odświeżać obraz w rytmie konsoli.

Jeśli ekran zostaje czarny, sprawdź zegar: przy złych wartościach PLL rdzeń chodzi, ale
na innym takcie, więc opóźnienia startowe przestają wystarczać. Jeśli pasy są poprzestawiane
albo widać śmieci, podejrzewaj SPI: przy 40 MHz złe prowadzenie przewodów potrafi psuć
transmisję, a wtedy trzeba zejść z taktem o jeden stopień w dół.

## Ćwiczenia

Zmień `BAND_H` z 8 na 4 i na 16. Zastanów się, co się dzieje z czasem: mniejsze pasmo
to więcej razy trzeba ustawiać okno panelu, większe to dłuższe czekanie na koniec transferu,
zanim zaczniemy następne. Osiem linii to kompromis między tymi dwiema stronami, dlatego
emulator wybrał właśnie tyle.

Zmień w `main.c` wartość, którą program wysyła rozkazem `MADCTL`, z `0x60` na `0xA0`.
Ten rozkaz ustawia orientację obrazu, więc po zmianie obraz obróci się o 180 stopni.
Znajdź biały kwadrat i sprawdź, w którym narożniku wylądował.

Dodaj do palety dwa własne kolory i użyj ich w pasach. Paleta ma osiem wpisów, bo tyle
wystarczy do ćwiczenia; w emulatorze będzie ich 64, po jednym na każdy kolor NES-a.

## Czego ten etap jeszcze nie ma

Emulacji. Bufor wypełniamy sami, wzorem, więc nie ma ani procesora 6502, ani układu
obrazu. Konwersja indeksów na kolory idzie przez procesor, bajt po bajcie, i przy pełnej
klatce zajmuje kilka milisekund. W etapie 15 zobaczysz, jak to samo zrobić szybciej:
emulator przerabia tylko te pasma, które się zmieniły, zamiast całej klatki za każdym razem.

## Co dalej

W etapie 05 zajrzymy do środka procesora: co robi z rozkazem, skąd wie, który rozkaz wykonać
jako następny i po co mu stos. Wynik zobaczymy na ekranie komputera, nie na panelu.
