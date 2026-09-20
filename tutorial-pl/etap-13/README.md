# Etap 13: kartridże większe niż pamięć

Program z etapu 08 był malutki, więc cały mieścił się w pamięci. Prawdziwe gry się nie
mieściły i nie dlatego, że ktoś napisał je nieporządnie. Nie miały gdzie się zmieścić.

Procesor ma szesnaście nóżek adresowych, czyli szesnaście wyprowadzeń, przez które wystawia
na zewnątrz numer adresu, o który prosi. Każda nóżka niesie jeden bit tego numeru, więc
szesnaście nóżek daje 65 536 różnych numerów i ani jednego więcej.

Zanim włożysz kartridż, część tej przestrzeni jest już zajęta: `$0000–$07FF` to pamięć
robocza procesora, czyli jego własne 2 kilobajty na zmienne i stos, a `$2000–$2007` to
rejestry układu obrazu. Kartridż odpowiada za górną połowę, od `$8000` w górę, czyli za
32 kilobajty. Gra z większą liczbą poziomów, grafiki i muzyki potrzebuje ich więcej.

## Kartridż kłamie, a procesor tego nie zauważa

To, co leży pod adresami `$8000–$BFFF`, zmienia się w trakcie działania programu. Procesor
widzi cały czas to samo okno, a kartridż decyduje, który kawałek siebie w nim pokazać.
Kawałek, który się pokazuje, nazywamy **bankiem**, a podmianę **przełączaniem banków**.

Górne okno, `$C000–$FFFF`, nigdy się nie zmienia, bo musi tam leżeć wektor resetu, czyli
adres, od którego procesor zaczyna po włączeniu zasilania. Gdyby i ten kawałek się
podmieniał, procesor nie wiedziałby, gdzie zacząć. Ten bank nazywamy stałym.

Zapis pod adres `$8000` albo wyżej nie trafia do pamięci, tylko do **rejestru**: liczba,
która tam trafi, mówi kartridżowi, który bank wstawić do okna. Ten sam mechanizm znasz
z układu obrazu, gdzie pod `$2000–$2007` też nie ma pamięci, tylko rejestry.

## Nasz kartridż

Kartridż tego etapu ma 48 kilobajtów, czyli trzy banki po 16 kilobajtów: bank 0 z kodem
rysującym górną połowę obrazu, bank 1 z kodem rysującym dolną, i bank stały z programem
startowym, paletą, wzorami kafli oraz wektorami.

Sam program jest malutki: każda jego połowa zajmuje w swoim banku 49 bajtów. To nie ma
znaczenia, bo liczy się zasada: obie połowy obrazu rysuje kod, którego nigdy nie ma w pamięci
naraz.
Zaglądnij do `cartridge.h`: tego pliku nikt nie napisał ręcznie, wygenerował go `host/asm.py`,
a leżą w nim tylko te bajty, których program naprawdę używa. Reszta banku to zera, czyli
puste miejsce na układzie. Prawdziwa gra zapełniłaby je po brzegi i dlatego potrzebowała
więcej niż jednego banku.

## Podmiana

Kartridż podmienia bank, kiedy dostanie zapis pod adres, który do niego należy. W `main.c`
robią to dwie linie:

```c
bank = (uint8_t)(value % CARTRIDGE_BANKS);
cartridge_window = bank_pointers[bank];
```

Dzielenie z resztą, to samo `%` co w etapie 12, sprowadza każdą wpisaną liczbę do numeru
jednego z banków. W kodzie dla 6502, w `asm.py`, podmiana to dwa rozkazy: `lda #1` i
`sta BANK_REG`. `BANK_REG` to adres rejestru kartridża, ten pod `$C000`: program pisze
właśnie tam, bo pod `$C000` stoi jego własny kod, który tę liczbę wysyła. Kartridż nic przy
tym nie kopiuje ani nie kasuje: poprzedni bank zostaje na układzie i tylko przestaje być
widoczny, dopóki ktoś nie zapisze z powrotem zera.

## Co robi gra

Pętla w `main.c` pozwala na każdą klatkę procesorowi wykonać część pracy, zamienia to, co
gra zapisała w pamięci układu obrazu, na piksele i wysyła obraz na panel. Gra w środku czeka
na początek klatki i rysuje obie połowy, a między nimi podmienia bank:

```asm
    lda #0
    sta BANK_REG        ; bank 0 w oknie
    jsr BANK_DRAW       ; górna połowa
    lda #1
    sta BANK_REG        ; podmiana
    jsr BANK_DRAW       ; dolna połowa
```

`jsr` to rozkaz wywołania podprogramu, ten sam, który znasz z etapu 05. Ten fragment
znajdziesz w `asm.py`, a `python3 asm.py --listing` wypisze cały program w tej postaci.

Każdy bank wypełnia swoją połowę siatki innym numerem kafla i zapisuje kolor do własnej
pozycji palety: bank 0 do pierwszej, bank 1 do drugiej. Bank 1 dodaje do koloru o dwa więcej
niż bank 0, więc obie pozycje różnią się o dwa przez cały czas działania programu.

## Co powinieneś zobaczyć

Ten etap pisze dla płytki, więc na komputerze zobaczysz tylko wynik sprawdzeń. Sam obraz ma
256 na 240 pikseli i dzieli się na dwie połowy. Górna jest gładka: jeden kafel bez wzoru, więc
widać jedną barwę na całej szerokości. Dolna to kafel w pionowe paski: co drugi piksel ma
barwę, a między nimi zostaje kolor tła, na panelu biały.

Barwa obu połówek zmienia się z każdą klatką, bo oba kafle biorą kolor z tej samej pozycji
palety, tej o numerze 1: tyle mówią ich piksele. Bank 0 pisze do niej co klatkę, a bank 1
pisze do pozycji numer 2, której żaden kafel nie używa, więc na panelu jej nie widać. Program
z tego katalogu czyta obie pozycje i on pokazuje, że bank 1 też wykonał swoją robotę.

Gra przesuwa dodatkowo obraz o piksel na klatkę. Po 256 klatkach obraz wraca do tego samego
wyglądu, bo wtedy i przewinięcie, i kolor zaczynają liczyć się od nowa.

## Jak to sprawdzić bez płytki

Ten etap ma trzy pliki źródłowe, a wspólny `tutorial-pl/Makefile` buduje po jednym, więc etap
ma własny `Makefile` z tymi samymi regułami. Z katalogu `etap-13` wystarczy `make check`,
a z katalogu `tutorial-pl` to samo robi `make STAGE=13 check`.

Sam kod na płytkę kompiluje się tak:

```bash
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 -Wall -Wextra -nostartfiles \
    -T ../../src/linker.ld main.c cpu.c ppu.c ../../src/startup_l476.s -o /tmp/e13.elf
```

Przechodzi bez jednego ostrzeżenia, a `arm-none-eabi-size /tmp/e13.elf` wypisuje `text 53156`,
`data 0` i `bss 64572`. Sam bufor obrazu to 61 440 bajtów, więc mieści się razem z pamięcią
emulowanego procesora i kartridżem w 96 kilobajtach pamięci płytki. Obrazu z tego etapu nie
zobaczysz na komputerze, ale to, co trafia na panel, sprawdzisz programem z tego katalogu:

```bash
cc -Wall -Wextra -I. host/host_check.c cpu.c ppu.c -o host/check
./host/check
```

Sprawdza on przez 600 klatek obraz, zanim trafi on na panel: górna połowa ma jeden kolor,
dolna dwa, a pozycje palety obu banków leżą o dwa od siebie. Gdyby kartridż przestał
podmieniać bank, dolna połowa wyszłaby z kafla banku 0 i oba warunki by się posypały.

## Ćwiczenia

Zamień w pętli głównej kolejność wywołań, tak żeby najpierw rysował bank 1. Obraz się nie
zmieni i to jest odpowiedź: każdy bank wpisuje swój numer do swojej połowy siatki i pisze do
własnej pozycji palety, a te dwa zapisy nic o sobie nie wiedzą. Sprawdź to okiem, a potem
wróć do poprzedniej kolejności.

Zmień w banku 0 liczbę w instrukcji `ldx #15` na `ldx #8`. Ten rejestr liczy wiersze siatki,
więc bank wypełni ich tylko osiem zamiast piętnastu. Nic jednak nie zniknie: pozostałe wiersze
wypełnił już przy starcie program z banku stałego i leży tam ten sam kafel. Pomyśl, co trzeba
by zmienić, żeby brak wypełnienia stał się widoczny.

Zmień w obu bankach `and #$0E` na `and #$0C`. Policz na kartce, które pozycje palety są
teraz możliwe, i sprawdź. Program przerwie się wtedy na komunikacie o pozycjach palety: obie
pozycje przestają leżeć o dwa od siebie.

Dodaj do kartridża trzeci bank. W `asm.py`, w funkcji `build`, dopisz `bank2, asm2 =
build_bank(2)` i dołóż go do tego, co ta funkcja zwraca, a potem uruchom `python3 host/asm.py`,
żeby zobaczyć wypisany kartridż. Skrypt nazwie trzeci bank `fixed`, tak samo jak bank stały,
więc łatwiej wzorować się na `bank_1` i dopisać tablicę `bank_2` w `cartridge.h` ręcznie.
W tym samym pliku dopisz nowy wskaźnik do `bank_pointers` i zmień `CARTRIDGE_BANKS` na 3.

Spraw, żeby oba banki zapisywały kolor pod ten sam adres palety: w `asm.py` zmień
`0x3F00 + 1 + bank` na `0x3F00 + 1`. Na ekranie zostanie wtedy kolor tego banku, który
zapisał jako drugi, a mimo to obraz pozostanie czytelny, bo kafle są różne. Odpowiedz,
dlaczego kolejność wywołań ma teraz znaczenie, choć w pierwszym ćwiczeniu nie miała.

## Co dalej

W etapie 14 weźmiemy kartridż, który nie poprzestaje na podmianie banków: dzieli ekran,
zmienia palety w trakcie rysowania obrazu i sam zgłasza przerwanie po wyznaczonym wierszu. To
układ, na którym wyszła największa gra z naszego dzieciństwa.
