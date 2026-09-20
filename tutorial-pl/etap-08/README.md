# Etap 08: układ obrazu, czyli kafle i palety

Do tej pory obraz powstawał z listy poleceń: pętla liczyła piksele i zapisywała je w buforze.
Dla wzoru testowego to wystarcza, dla gry nie. Kartridż nie ma gdzie trzymać takiego obrazu,
bo ekran konsoli to 61 440 pikseli: 256 w poziomie i 240 w pionie. A do tego obraz w grze
prawie się nie zmienia.

Konsola trzyma więc nie obraz, a jego opis. Opis to trzy rzeczy: wzory kafli, mapa z tablicą
atrybutów i paleta. Ten etap zamienia je na piksele. Pod koniec zobaczysz na panelu pierwszy
prawdziwy obraz z kartridża.

## Kafel, czyli wzór osiem na osiem

Najmniejszy kawałek obrazu, jaki konsola umie narysować, ma osiem na osiem pikseli i nazywa
się **kaflem**. Kafel to nie obrazek, tylko szesnaście bajtów. Pierwszych osiem opisuje jedną
warstwę kafla, drugie osiem drugą. W `cartridge.h` warstwa nazywa się *bit plane*, bo trzyma
po jednym bicie na piksel. Każdy piksel bierze po jednym bicie z obu warstw, więc jego wartość
to liczba od zera do trzech: zero znaczy „żadnego koloru", a jeden, dwa i trzy to trzy kolory
z palety.

Weź literę A z pamięci grafiki naszego kartridża, tego z etapu 07. Jej pierwsza warstwa to
liczby 0x38, 0x44, 0x44, 0x7C, 0x44, 0x44, 0x44, 0x00. Bajt 0x38 to bity 00111000, a pierwszy
bit należy do piksela w lewym górnym rogu. Czytamy je od lewej, jak tekst, i dostajemy osiem
wierszy litery:

    ..###...
    .#...#..
    .#...#..
    .#####..
    .#...#..
    .#...#..
    .#...#..
    ........

Druga warstwa tej litery to same zera, więc jej piksele mają wartość zero albo jeden. W tym
kartridżu żaden kafel nie używa drugiej warstwy, więc każdy wzór ma jeden kolor i tło.

## Mapa kafli, czyli cały ekran w 960 bajtach

Obraz konsoli to 32 kafle w poziomie i 30 w pionie, czyli 960 kafli. Wystarczy więc tablica
960 liczb: pierwsza mówi, co stoi w lewym górnym rogu, druga, co obok niej, i tak dalej. Taka
tablica nazywa się **nametable** i pod tą nazwą występuje w `cartridge.h` oraz w kodzie
emulatora.

Popatrz, ile to oszczędza. Te same kafle zapisane jako piksele zajęłyby 61 440 bajtów, czyli
sześćdziesiąt cztery razy więcej. Powtarzające się wzory są zapisane raz. W pliku
`cartridge.h` wiersze tablicy mają komentarze, więc sprawdzisz, że 0x17, 0x13 i 0x18 to
litery M, I, N.

## Paleta, czyli skąd kolor

Kafel mówi, gdzie jest kolor, a nie jaki. Kolor wybiera **paleta**: cztery liczby, jedna na
tło i trzy na pozostałe wartości piksela. Konsola trzyma cztery takie palety dla tła, czyli
szesnaście bajtów, a drugie tyle dla duszków, czyli małych ruchomych obrazków.

Sama paleta też nie trzyma kolorów, tylko numery od zera do 63. Konsola wysyła numer
przewodem, a jak ten numer wygląda, decyduje telewizor. W kodzie tego etapu jest więc tablica
`colour_rgb565` ze wszystkimi 64 numerami i ich kolorami dla panelu. Jedna rzecz bywa przy
tym zaskakująca: wartość piksela równa zero nie znaczy „pierwszy kolor z palety", tylko
„żadnego koloru". Konsola maluje tam wspólny kolor tła, ten sam dla wszystkich czterech palet,
i dlatego każda paleta w `cartridge.h` zaczyna się od bajtu 0x0F, czyli czerni.

## Atrybuty, czyli która paleta gdzie

Zostało powiedzieć, która paleta obowiązuje w którym miejscu ekranu. Służy do tego tablica
atrybutów: 64 bajty, leżące w tej samej pamięci konsoli co mapa, zaraz za nią. Przypada
po jednym bajcie na pole 4 na 4 kafle, czyli 32 na 32 piksele. Jeden bajt niesie cztery
numery palet, po dwa bity na ćwiartkę pola, a ćwiartka to 2 na 2 kafle, czyli 16 na 16 pikseli.

Uproszczenie ma swoją cenę: paleta zmienia się tylko na granicy ćwiartki pola, nigdy w środku
kafla. Widać to na naszym obrazie. Wiersz `OVER THE LAZY DOG. 1234567890` łamie się na kolory
co cztery kafle, czyli co 32 piksele, a granica wypada w środku wyrazu: `OV` jest
jasnoniebieskie, a `ER` czerwone.

## Skąd te dane

Trzy rzeczy leżą w pliku `cartridge.h`: wzory kafli w tablicy `chr_rom`, mapa z atrybutami
w `nametable` i `attributes`, oraz paleta w `bg_palette`. Wzory pochodzą wprost z pamięci
grafiki tego kartridża, którego nagłówek czytaliśmy w etapie 07. Reszta to zrzut pamięci
obrazu konsoli, czyli to, co program gry wpisał tam na początku; programu gry jeszcze nie
uruchamiamy, więc obraz jest zamrożony.

Z tych danych obraz składa osobny układ konsoli, czyli **układ obrazu**. W dokumentacji i w
kodzie emulatora nazywa się PPU, od angielskiego *Picture Processing Unit*. Nasz program robi
to samo, tylko prościej: przechodzi mapę kafel po kaflu, kopiuje wzór do bufora obrazu
z etapu 04 i podstawia kolory z palety wskazanej przez atrybuty. Bufor idzie na panel pasmami,
jak w etapie 04.

## Co powinieneś zobaczyć

Czarny ekran, a na nim jasnoniebieski napis `MINI MARIO NES TEST`, wiersz `FRAME:`, napis
`SHAPES` i osiem kształtów: cegiełkę, monetę, serce, kratkę, klepsydrę, znak zapytania,
kępkę trawy i ramkę. Pod nimi stoi `PALETTES` i cztery grupy czerwonych kwadratów, a pod
kwadratami siedem wierszy tekstu: trzy czerwone, jeden łamie się co 32 piksele na czerwony
i jasnoniebieski, dwa jasnoniebieskie i ostatni zielony.

Kolorów jest na całym ekranie cztery: czarny, czerwony, zielony i jasnoniebieski. Tyle
wychodzi z trzech palet użytych przez ten obraz. Obraz konsoli jest węższy od panelu, więc
po bokach zostają czarne pasy, tak jak w etapie 04. Puste miejsce nad napisem `FRAME:` to nie
błąd: konsola wpisuje tam dwie cyfry licznika klatek, ale dopiero wtedy, gdy obraz zaczyna się
odświeżać.

## Zbuduj i sprawdź

Z katalogu `tutorial-pl` uruchom `make STAGE=08 check`. Ta komenda tylko kompiluje kod
i nic nie wgrywa, a kompilacja przechodzi bez ostrzeżeń przy `-Wall -Wextra`; to cała
weryfikacja tego etapu. Na płytkę wgrywasz ten sam kod komendą `make STAGE=08 flash`;
obrazu nie zobaczysz na komputerze.

## Ćwiczenia

W `cartridge.h` zamień miejscami dwa numery w `bg_palette`, na przykład 0x21 i 0x16. Napis
zmieni kolor, choć żaden kafel się nie zmienił. To cała różnica między kształtem a kolorem.

Wstaw do `nametable` kafel trawy, czyli 0x31, w miejsce pierwszej spacji w wierszu 0. Kępka
trawy pojawi się tam, gdzie jej nie było. Tak gry budują plansze: jedna liczba na jedno pole.

Zamień w `cartridge.h` pierwszy bajt tablicy `attributes` z 0x00 na 0x55. Kolor zmieni się nie
w jednym kaflu, ale na całym polu 32 na 32 piksele: w połowie wyrazu `MINI` i w literach `FR`
z `FRAME:`.

Zrób własny wzór. Pierwszy kafel w `chr_rom`, numer zero, jest pusty: wpisz mu w pierwszej
warstwie osiem razy 0xFF i postaw kafel 0 w dowolnym miejscu `nametable`. Osiem pełnych
wierszy to pełny kwadrat, a pojedyncze bity w nich zmienią go w inny kształt.

## Co dalej

Obraz stoi w miejscu, bo wszystko, co widzisz, pochodzi z tabel, których nikt nie zmienia.
W etapie 09 dojdą duszki, czyli osobna lista małych obrazków z własnymi pozycjami; konsola
rysuje je nad tłem i można je przesuwać.
