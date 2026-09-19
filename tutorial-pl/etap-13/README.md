# Etap 13: kartridże większe niż pamięć

Program z etapu 08 zmieścił się w pamięci, bo był malutki. Prawdziwe gry się nie mieściły
i nie dlatego, że ktoś napisał je nieporządnie. Nie miały gdzie się zmieścić.

Procesor ma szesnaście nóżek adresowych, czyli szesnaście wyprowadzeń, przez które wystawia
na zewnątrz numer adresu, o który prosi. Każda nóżka niesie jeden bit tego numeru, więc
szesnaście nóżek daje 65 536 różnych numerów i ani jednego więcej.

Zanim włożysz kartridż, ta przestrzeń jest już zajęta: `$0000–$07FF` to pamięć robocza
procesora, czyli jego własne 2 kilobajty na zmienne i stos, `$2000–$2007` to rejestry układu
obrazu, a resztę dostaje kartridż. Zostaje mu 32 kilobajty, a gra z większą liczbą poziomów,
grafiki i muzyki potrzebuje ich więcej.

## Kartridż kłamie, a procesor tego nie zauważa

To, co leży pod adresami `$8000–$BFFF`, zmienia się w trakcie działania programu. Procesor
widzi cały czas to samo okno, a kartridż decyduje, który kawałek siebie w nim pokazać.
Kawałek, który się pokazuje, nazywamy **bankiem**, a podmianę **przełączaniem banków**.

Górne okno, `$C000–$FFFF`, nigdy się nie zmienia, bo musi tam leżeć wektor resetu, czyli
adres, od którego procesor zaczyna po włączeniu zasilania. Gdyby i ten kawałek się
podmieniał, procesor nie wiedziałby, gdzie zacząć. Ten bank nazywamy stałym.

Adres `$8000` i wyżej to przy tym nie pamięć, do której się pisze, ale **rejestr**:
zapisanie pod ten adres liczby mówi kartridżowi, który bank wstawić do okna. Ten sam
mechanizm znasz z układu obrazu, gdzie pod `$2000–$2007` też nie ma pamięci, tylko rejestry.

## Nasz kartridż

Kartridż tego etapu ma 48 kilobajtów, czyli trzy banki po 16 kilobajtów: bank 0 z kodem
i danymi na górną połowę obrazu, bank 1 z tym samym na dolną, i bank stały z programem
startowym, paletą, wzorami kafli oraz wektorami.

Sam program jest malutki: każda jego połowa zajmuje w swoim banku 49 bajtów. To nie ma
znaczenia, bo liczy się zasada: obie połowy obrazu rysuje kod, którego nigdy nie ma w pamięci
naraz.
Zaglądnij do `cartridge.h`: tego pliku nikt nie napisał ręcznie, wygenerował go `host/asm.py`,
a wypisane są w nim tylko bajty, których program naprawdę używa. Reszta banku to zera, czyli
puste miejsce na układzie. Prawdziwa gra zapełniłaby je po brzegi, i dlatego potrzebowała
więcej niż jednego banku.

## Podmiana

Kartridż podmienia bank, kiedy dostanie zapis pod adres, który do niego należy. W `main.c`
wystarczy do tego przypisanie `cartridge_window = bank_pointers[value % CARTRIDGE_BANKS]`,
a w kodzie dla 6502, w `asm.py`, dwa rozkazy: `lda #1` i `sta BANK_REG`. `BANK_REG` to adres
rejestru kartridża, ten pod `$C000`. Nic nie jest przy tym kopiowane ani kasowane: poprzedni
bank zostaje na układzie i tylko przestaje być widoczny, dopóki ktoś nie zapisze z powrotem
zera.

## Co robi gra

Pętla w `main.c` pozwala na każdą klatkę procesorowi wykonać część pracy, zamienia pamięć
obrazu na piksele i wysyła obraz na panel. Gra w środku czeka na początek klatki i rysuje
obie połowy, a między nimi podmienia bank:

```asm
    lda #0
    sta BANK_REG        ; bank 0 w oknie
    jsr BANK_DRAW       ; górna połowa
    lda #1
    sta BANK_REG        ; podmiana
    jsr BANK_DRAW       ; dolna połowa
```

`jsr` to rozkaz wywołania podprogramu, ten sam, który znasz z etapu 05. Ten fragment
znajdziesz w `asm.py`, a `--listing` wypisze cały program w tej postaci.

Każdy bank wypełnia swoją połowę siatki innym numerem kafla, a kolor, który odczytał
z rejestru banku, zapisuje w swojej pozycji palety. Ten odczyt jest dowodem, o który tutaj
chodzi: program czyta spod adresu `$C000` i dostaje to, co podstawił kartridż.

## Co powinieneś zobaczyć

Obraz 256 na 240 pikseli, podzielony na dwie połowy. Górna jest gładka: jeden kafel, jedna
barwa na całej szerokości, i ta barwa zmienia się z każdą klatką. Dolna jest w pionowe paski,
bo składa się z kafla w szachownicę, i zmienia kolor razem z górną, zawsze o dwie pozycje
palety dalej. Obraz przesuwa się w prawo o piksel na klatkę, więc po czterech sekundach
zatacza koło.

## Jak to sprawdzić bez płytki

Ten etap ma trzy pliki źródłowe, a wspólny `tutorial-pl/Makefile` buduje po jednym, więc etap
ma własny `Makefile` z tymi samymi regułami. Z katalogu `etap-13` wystarczy `make check`.

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

Sprawdza on to samo, co widać na panelu: przez 600 klatek górna połowa ma jeden kolor,
dolna dwa, a pozycje palety obu połówek trzymają się dwie pozycje od siebie. Ten drugi
warunek sprawdza odczyt rejestru banku: gdyby oba banki czytały to samo, liczby
przestałyby się różnić.

## Ćwiczenia

Zamień kolejność w pętli głównej, tak żeby najpierw rysował bank 1. Dolna połowa zrobi się
gładka, a górna w paski, bo kafle nie zależą od kolejności: każdy bank zawsze wpisuje swój
numer do swojej połowy siatki. Zmienia się tylko to, który kolor trafia na wierzch.

Zmień w banku 0 liczbę w instrukcji `ldx #15` na `ldx #8`. Ten rejestr liczy wiersze siatki,
więc bank wypełni ich tylko osiem zamiast piętnastu. Nic jednak nie zniknie: pozostałe wiersze
wypełnił już przy starcie program z banku stałego i leży tam ten sam kafel. Pomyśl, co trzeba
byłoby zmienić, żeby brak wypełnienia stał się widoczny.

Zmień w obu bankach `and #$0E` na `and #$0C`. Policz na kartce, które pozycje palety są
teraz możliwe, i sprawdź. Warunek, który porównuje pozycje obu połówek, przestanie wtedy
przechodzić.

Dodaj do kartridża trzeci bank: w `asm.py` dopisz `build_bank(2)` tam, gdzie skrypt buduje
pozostałe dwa banki, i uruchom `python3 asm.py`, żeby zobaczyć wypisany kartridż. Skrypt
nazywa trzeci bank `fixed`, więc łatwiej wzorować się na `bank_1` i dopisać tablicę `bank_2`
w `cartridge.h` ręcznie. Do `bank_pointers` w tym samym pliku dopisz nowy wskaźnik, a w
`main.c` zmień `CARTRIDGE_BANKS` na 3.

Spraw, żeby oba banki zapisywały kolor pod ten sam adres palety. Obie połowy dostaną wtedy
ten sam kolor, a mimo to obraz pozostanie czytelny, bo kafle są różne. Odpowiedz, dlaczego
to, co zostaje na ekranie, zależy od tego, który bank rysował jako drugi.

## Co dalej

W etapie 14 weźmiemy kartridż, który nie poprzestaje na podmianie banków: dzieli ekran,
zmienia palety w trakcie linii obrazu i sam zgłasza przerwanie po wyznaczonej linii. To
układ, na którym wyszła największa gra z naszego dzieciństwa.
