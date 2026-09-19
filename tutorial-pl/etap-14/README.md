# Etap 14: najbardziej złożony kartridż, czyli jak jeden układ dzieli ekran

Etap 13 skończył się na kartridżu, który podmienia banki pamięci, żeby procesor widział więcej
programu, niż mieści się pod jego adresami. Procesor sięga wtedy pod ten sam adres co zawsze,
a kartridż podstawia pod niego inny fragment swojej pamięci. To była sztuczka na pamięci. Ten
etap bierze ten sam pomysł i dokłada do niego trzy rzeczy dotyczące obrazu: dzielenie ekranu
na dwie części, które mogą poruszać się niezależnie, paletę wybieraną dla każdego kafla osobno
i licznik linii obrazu, który przerywa procesorowi pracę w połowie klatki.

Układ nazywa się **MMC5**: MMC to skrót, którym oznacza się układy zarządzające pamięcią
kartridża, a piątka mówi, który to model z tej rodziny. Powstał dla dużych gier i jedna z nich,
Castlevania III: Dracula's Curse, wyszła na kartridżu z tym układem w środku. Nowy kod jest
znowu programem na komputer, nie na płytkę: ten etap sprawdza układ, więc zamiast programu do
wgrania dostajesz plik z obrazem i wydruk sprawdzeń. Kod leży w czterech plikach: `mmc5.c` to sam
układ, `ppu.c` to układ obrazu z etapów 08 i 12, `cpu.c` to procesor z etapu 13, a `main.c` składa
z nich maszynę, grę i sprawdzenia. Z katalogu `etap-14` buduje się to tak:

    cc -Wall -Wextra main.c cpu.c ppu.c mmc5.c -o etap14 -lz
    ./etap14

Kompilator przechodzi to bez ani jednego ostrzeżenia, a program wypisuje, co zrobił, i zapisuje
ostatnią klatkę do pliku `etap14.png`. W tym samym katalogu `make` buduje program, a `make check`
buduje go, uruchamia i sprawdza wynik. Wspólny `Makefile` o katalog wyżej sięga po kompilator
dla procesorów Arm i po plik startowy płytki, a ten program potrzebuje biblioteki `zlib`, której
w zestawie dla Arm nie ma. Dlatego `make STAGE=14 check` z katalogu `tutorial-pl` kończy się na
`fatal error: zlib.h: No such file or directory`: to nie jest etap na płytkę.

## Kartridż, który odpowiada na pytania

W etapie 12 układ obrazu rysował jedną linię po drugiej. Tutaj ta sama pętla zadaje przy każdym
kaflu pytanie do kartridża: co narysować w tym miejscu? W kodzie to jedna funkcja,
`mmc5_background`, i dwie struktury: pytanie oraz odpowiedź. Pytanie niesie pozycję na ekranie,
więc odpowiedź może ją zignorować. Odpowiedź mówi, który to kafel, z której palety wziąć kolory
i z którego banku wzorów, czyli z którego kawałka pamięci grafiki kartridża.

Na tym jednym wywołaniu stoi cały etap. Skoro kartridż widzi każdy kafel osobno, może odpowiadać
inaczej dla lewej i prawej części tej samej linii, a zwykły kartridż tego nie potrafi: oddaje
pamięć i milknie.

## Ekran dzielony na dwie części

Rejestr `$5200` włącza podział i mówi, po której stronie ekranu i od którego kafla się zaczyna.
`$5201` to pionowa pozycja tej części obrazu, a `$5202` jej własny bank wzorów. W tej grze prawa
strona, od kafla 26, czyli od piksela 208, pokazuje drugą nametable, czyli drugą tablicę kafli,
taką samą jak w etapie 08. Siedzi w niej pasek z wynikiem; w kodzie i w wydruku sprawdzeń ten
pasek nazywa się `panel`.

Świat wspina się o piksel na klatkę, bo gra zapisuje nową wartość do rejestru przewijania
`$2005`. Pasek nie drgnie, bo nie słucha tego rejestru: rysuje go kartridż, z własną pozycją.
Sprawdzenie w programie rusza samym `$5201` i patrzy, czy pasek się przesunął, a świat ruszył się
tylko o swój zwykły piksel.

## Paleta dla każdego kafla

Układ ma kilobajt własnej pamięci, po jednym bajcie na kafel tła. Dwa bity to numer palety,
a pozostałe sześć to numer banku wzorów. Włącza to zapis do `$5104`. Linia z pochodnią niesie
wtedy dwie palety naraz: szary kamień i ogień. Nocne niebo w oknach i napisy paska to dwie
następne, każda w swoim miejscu obrazu.

Bez tego bajtu obowiązuje to, co znasz z etapu 08: jeden bajt tablicy atrybutów na pole 4 na 4
kafle. Jedno ze sprawdzeń wyłącza pamięć układu i ściana wraca wtedy do palety nocnego nieba:
traci kamień i pochodnie.

Do tej pamięci procesor może pisać tylko wtedy, gdy obraz jest właśnie rysowany. To nie kaprys:
dzięki temu obsługa przerwania jest jedynym dobrym momentem na zmianę jej zawartości.

## Licznik linii i przerwanie

`$5203` to liczba linii, po której układ ma pociągnąć przewód przerwania, a `$5204` pozwala temu
przerwaniu przejść. Licznik liczy linie w trakcie rysowania. Kiedy dojdzie do celu, układ podnosi
flagę, a procesor skacze pod adres obsługi, który kartridż trzyma dla tego sygnału.

Obsługa przerwania robi dwie rzeczy. Najpierw czyta `$5204`, bo tylko odczyt kasuje flagę. Potem
wpisuje inną barwę do palety. Trafia w sam środek klatki, więc wszystko poniżej linii 200 jest
rysowane ciepłym kolorem. Gdyby obsługa nie odczytała rejestru, flaga stałaby podniesiona
i procesor wracałby do obsługi raz po raz, nie robiąc nic innego. Program sprawdza i to.

Gra co klatkę przywraca ten wpis palety ze swojej tablicy, a tablica leży w drugim banku
kartridża, pod adresem `$A005`. Działa tu ten sam mechanizm okien bankowych co w etapie 13:
rejestr `$5117` mówi, który bank kartridża pokazuje się pod adresem `$A000`, a paleta leży w tym
banku na początku, więc jej szósty bajt wypada pod `$A005`.

## Gra w środku

Program procesora to 181 bajtów w ostatnim banku, wypisanych w `main.c` wiersz po wierszu: adres,
bajty i rozkaz. Ma trzy części: ustawienie rejestrów obu układów, pętlę na jedną klatkę i obsługę
przerwania. Zmienne trzyma w dwóch bajtach strony zerowej, czyli na samym początku pamięci: `$00`
to pozycja przewijania świata, a `$01` liczy klatki dla kroku bohatera. Procesor ma dla tego
początku krótsze rozkazy, więc trzymanie tam zmiennych jest tańsze.

Tę grę napisałem na potrzeby rozdziału, żeby było widać każdy mechanizm układu osobno; duża gra
używa tych trzech mechanizmów naraz i w wielu miejscach. Obraz nie jest tu robotą programu: wzory
kafli, mapa, paleta i poziom czekają gotowe, zanim procesor wystartuje. Rozdział poświęca swoje
miejsce układowi, a nie wgrywaniu obrazka.

## Co powinieneś zobaczyć

Otwórz `etap14.png`. Obraz to mur z cegieł, z filarami co osiem kolumn, oknami i pochodniami,
a po prawej, od piksela 208, pasek z napisami `CASTLE`, `SCORE 001250`, `LIVES 3`, `FLOOR 2`,
`MMC5` i `SPLIT ON`. Dolne czterdzieści linii ma ciepły odcień, bo tam dotarła zmiana palety.
Bohater, czyli duszek z etapu 09, stoi w połowie wysokości i zmienia krok co osiem klatek.

Program wypisuje też linię `frame 40, the world scrolled 40 pixels, interrupts in it: 1
(line 200)` oraz zdanie `the panel is columns 208..255 of the picture`; do tego siedemnaście
sprawdzeń, każde z `ok`. Liczby mówią, że pętla gry obróciła się czterdzieści razy, a przerwanie
przyszło raz na klatkę, tam gdzie miało.

## Ćwiczenia

Wszystkie zmiany robisz w tablicy `game_code` w `main.c`. Każdy wiersz to jeden rozkaz: adres,
bajty i nazwa rozkazu. Pierwszy bajt to numer rozkazu, drugi to jego argument, więc podmieniasz
ten drugi.

**Przesuń granicę światła.** W wierszu `[0x0023] = 0xA9, 0xC8,` drugi bajt to liczba linii, którą
program wpisuje do `$5203` (200). Wpisz tam `0x78`, czyli 120: ciepła część obrazu podniesie się
o 80 linii, a dwa sprawdzenia przestaną przechodzić, bo pilnują tej liczby.

**Daj paskowi własne przewijanie.** Wiersz `[0x0014] = 0xA9, 0x00,` to rozkaz `lda #0` przed
zapisem do `$5201`. Wpisz `0x08` zamiast `0x00`. Pasek przesunie się o jeden kafel, a świat
zostanie na miejscu. Trzy sprawdzenia przestaną przechodzić: jedno pilnuje pozycji paska, a dwa
szukają jego napisów tam, gdzie stały.

**Zmień kolor, który wpisuje obsługa.** Wiersz `[0x00AA] = 0xA9, 0xB2,` to `lda #$B2` przed
zapisem do palety. Wpisz na przykład `0x1C`, czyli zielony. Dolna część obrazu zmieni barwę,
górna nie. Jedno sprawdzenie przestanie przechodzić, bo szuka w obrazie starej barwy.

**Wyłącz podział ekranu.** Wiersz `[0x000F] = 0xA9, 0xDA,` to `lda #$DA` przed zapisem do
`$5200`. Najstarszy bit włącza podział. Wpisz `0x5A`, żeby go zgasić: pasek zniknie, a jego
miejsce zajmie dalsza część muru. Trzy sprawdzenia przestaną przechodzić: jedno sprawdza, czy
pasek stoi w miejscu, a dwa szukają jego napisów.

**Podstaw zaślepkę za obsługę przerwania.** Na końcu tablicy stoją trzy adresy, pod które skacze
procesor; adres obsługi przerwania układu to `[0x1FFE] = 0x9D, [0x1FFF] = 0xE0,`. Wpisz tam
`0xB0` i `0xE0`, czyli adres `stub_ack`: przerwanie przyjdzie, ale paleta zostanie nietknięta,
więc jedno sprawdzenie przestanie przechodzić.

## Co dalej

W etapie 15 zmierzymy, gdzie znika czas. Ta sama gra i ten sam obraz, ale z licznikiem taktów
w środku: najpierw pomiar, a potem dwie albo trzy zmiany wybrane na jego podstawie. Zobaczysz
tę samą grę wyraźnie szybszą i tabelę, która mówi, skąd wzięła się różnica.
