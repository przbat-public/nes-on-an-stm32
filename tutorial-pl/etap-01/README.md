# Etap 1: zegar, bufor obrazu i pasma

W etapie 0 kolor leciał wprost na panel. Nie było pośrednika, więc nie było czego
przerysować, z czym porównać ani na czym zaoszczędzić. Ten etap wprowadza trzy rzeczy,
na których stoi cała ścieżka obrazu w emulatorze: szybszy zegar, bufor obrazu i wysyłanie
pasmami.

## Najpierw zegar, bo bez niego nie ma czasu

Mikrokontroler startuje na wewnętrznym zegarze około 4 MHz. Przy takim takcie na każdy
piksel zostaje tyle czasu, że nie da się między nimi zrobić nic sensownego. Pętla PLL
podnosi go do 80 MHz, czyli dwadzieścia razy więcej.

Arytmetyka jest zapisana w kodzie i warto ją przeliczyć samodzielnie: źródłem jest HSI16,
czyli 16 MHz, mnożnik N równa się 10, a dzielnik R równa się 2. Szesnaście razy dziesięć
daje 160 MHz na wyjściu oscylatora, a podzielone przez dwa daje 80 MHz dla rdzenia. Trzy
liczby to trzy pola jednego rejestru, więc zmiana któregokolwiek z nich zmienia takt
całego układu, nie tylko SPI.

Przy 80 MHz sama pamięć flash nie nadąża za rdzeniem. Dlatego przed włączeniem PLL
ustawiamy cztery stany oczekiwania i włączamy pamięci podręczne. Jeśli to pominiesz,
program będzie działał, ale wolniej i w sposób, którego nie da się przewidzieć.

## Bufor obrazu, czyli dlaczego jeden bajt na piksel

Panel przyjmuje kolory w formacie RGB565, czyli dwa bajty na piksel. Gdybyśmy trzymali
w pamięci to, co panel, obraz 256 na 240 pikseli zajmowałby 120 kilobajtów. Mikrokontroler
ma 96 kilobajtów RAM, więc taki bufor się nie mieści.

Dlatego bufor trzyma **indeksy do palety**: jeden bajt na piksel, a kolor powstaje dopiero
w chwili wysyłania. Ten sam obraz zajmuje wtedy 60 kilobajtów, a paleta to 64 wpisy po dwa
bajty, czyli 128 bajtów. Zamiana indeksu na kolor kosztuje jedno odwołanie do tablicy
na piksel i to jest cała cena tego rozwiązania.

Bufor obejmuje tylko obraz z konsoli, 256 na 240. Panel ma 320 pikseli szerokości, więc
po bokach zostają dwa czarne pasy po 32 piksele. Malujemy je raz przy starcie i nigdy
więcej ich nie dotykamy.

## Pasma, czyli jak ukryć transmisję

Cały obraz to 122 880 bajtów. Przy 40 MHz na drucie zajmuje to 24,6 milisekundy, licząc
razem z bitami startu i stopu na każde osiem bitów danych. To dużo, jeśli porównać
z czasem, jaki emulator potrzebuje na policenie jednej klatki.

Dlatego obraz nie leci jednym kawałkiem. Dzielimy go na pasma po osiem linii, czyli
po 4096 bajtów, i wysyłamy pasmo po paśmie. Gdy jedno pasmo jedzie po SPI, procesor
w tym samym czasie ma wolne i może liczyć następne. W etapie 1 nie ma jeszcze czego
liczyć, więc pasma są tylko pokazówką mechanizmu. W etapie 4, kiedy pojawi się PPU,
z tego czasu skorzysta emulacja i właśnie dlatego gra chodzi, zamiast czekać na ekran.

## Wzór testowy

Ekran wypełnia osiem pionowych pasów kolorów, a w lewym górnym narożniku siedzi biały
kwadrat. Ta asymetria jest celowa. W ćwiczeniu z etapu 0 odkryłeś, że jednolity kolor
ukrywa obrót panelu o 180 stopni. Teraz nie ukryje: jeśli biały kwadrat pojawi się
w innym narożniku, niż powinien, wiesz, że orientacja jest ustawiona inaczej.

## Zbuduj i wgraj

```bash
# z katalogu tutorial-pl:
make STAGE=01 flash
```

To samo ręcznie, jeśli chcesz zobaczyć każdy krok:

```bash
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 -nostartfiles \
    -T ../../src/linker.ld ../../src/startup_l476.s main.c -o etap01.elf
arm-none-eabi-objcopy -O binary etap01.elf etap01.bin
st-flash write etap01.bin 0x08000000
st-flash reset
```

## Co powinieneś zobaczyć

Osiem pionowych pasów i biały kwadrat w lewym górnym narożniku. Wypełnienie ekranu
powinno być teraz wyraźnie szybsze niż w etapie 0, mimo że obraz jest większy i trzeba
go jeszcze przekonwertować.

Jeśli ekran zostaje czarny, sprawdź zegar: przy złych wartościach PLL rdzeń chodzi, ale
na innym takcie, więc opóźnienia startowe przestają wystarczać. Jeśli pasy są poprzestawiane
albo widać śmieci, podejrzewaj SPI: przy 40 MHz złe prowadzenie przewodów potrafi psuć
transmisję, a wtedy trzeba zejść z taktem o jeden stopień w dół.

## Ćwiczenia

Zmień `BAND_H` z 8 na 4 i na 16. Zastanów się, co się dzieje z czasem: mniejsze pasmo
to częstsze przerwania w transmisji i więcej rozkazów ustawiających okno, większe pasmo
to dłuższe czekanie na koniec transferu, zanim zaczniemy następne. Emulator wybrał 8 linii
właśnie jako kompromis między tymi dwiema stronami.

Zmień `MADCTL` z `0x60` na `0xA0` i znajdź biały kwadrat. To ten sam obrót o 180 stopni,
którego w etapie 0 nie dało się zobaczyć.

Dodaj do palety dwa własne kolory i użyj ich w pasach. Paleta ma osiem wpisów, bo tyle
wystarczy do ćwiczenia; w emulatorze będzie ich 64, po jednym na każdy kolor NES-a.

## Czego ten etap jeszcze nie ma

Emulacji. Bufor wypełniamy sami, wzorem, więc nie ma ani procesora 6502, ani układu
obrazu. Konwersja indeksów na kolory idzie przez procesor, bajt po bajcie, i przy pełnej
klatce zajmuje kilka milisekund. W etapie 12 zobaczysz, jak to samo zrobić szybciej,
i dlaczego wtedy warto porównywać pasma z tym, co panel już ma, zamiast wysyłać je
bezmyślnie co klatkę.
