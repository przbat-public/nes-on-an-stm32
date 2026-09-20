# Etap 05: czym jest procesor, czyli rejestry, rozkazy i stos

Panel pokazywał obraz, ale nikt nie namalował go ręcznie: powstał z listy poleceń, którą wykonał
mikrokontroler. Ten etap zostawia płytkę w pudełku i pyta, co to znaczy, że coś wykonuje program.
Wynik wypisujemy tym razem na ekranie komputera, funkcją `printf` z biblioteki języka C. To jej
programy na komputerze używają, żeby wypisać tekst: bierze tekst, podstawia pod niego wartości
i wysyła go na ekran.

## Procesor to kilka rejestrów i pętla

Rejestr znasz z etapu 03: miejsce w układzie, które ma adres i swoje przełączniki. Rejestry
procesora rządzą się inną zasadą: jest ich kilka, siedzą wewnątrz procesora i nie mają adresów,
bo procesor sięga do nich bez pytania nikogo o pozwolenie. Jeden trzyma liczbę, na której
liczymy (`A`), drugi służy za licznik (`X`). Osobno jest **licznik rozkazów** (program counter,
w kodzie `pc`): liczba mówiąca, którą instrukcję wykonać jako następną. Cała reszta to pętla:
weź rozkaz spod adresu w `pc`, zrób to, co mówi, wróć po następny.

Ten procesor to **6502**, ten sam, który siedzi w konsoli z tego przewodnika. Prawdziwy 6502 ma
151 rozkazów; nasz ma dwanaście, wybranych tak, żeby dało się coś policzyć.

## Rozkaz to liczba

Program jest ciągiem bajtów: rozkaz zajmuje jeden bajt i mówi, co zrobić (w dokumentacji taki
bajt nazywa się **opcode**), a za nim jadą dane:

```
adres  bajty       rozkaz     co robi
0000   01 00       LDA #0     A = 0, wynik na początek
0002   02 06       LDX #6     X = 6, tyle razy dodamy siedem
0004   03 07       ADC #7     A = A + 7
0006   05          DEX        X = X - 1
0007   0A 04 00    BNE 0004   X nie jest jeszcze zerem? wróć na 0004
000A   0C          HLT        koniec programu
```

Krzyżyk znaczy „liczba podana przy rozkazie", a nie adres: `LDA #0` wpisuje do `A` zero, które
program wiezie ze sobą. Pierwszy rozkaz leży pod adresem 0000 i stamtąd startuje licznik
rozkazów, dlatego adresy w tabeli to pozycje w tym programie.

Po 0009 nie następuje 0010, a 000A, bo adresy zapisujemy szesnastkowo: po dziewiątce idzie
litera `A`, która znaczy dziesięć. `BNE` zajmuje trzy bajty, bo po nim jadą dwa bajty adresu:
najpierw młodszy, potem starszy, i stąd `HLT` wypada pod 000A.

Z tego wynika rzecz, do której będziemy wracać: adresy to pozycje w liście. Wstaw w środek
programu rozkaz, a skok poniżej zacznie prowadzić gdzie indziej. Asembler, czyli program
tłumaczący nazwy rozkazów na bajty, powstał właśnie po to, żeby przeliczać te adresy za nas.

Nazwy rozkazów są prawdziwe, numery już nie: `LDA` to „load into A", czyli wczytaj do `A`,
a `ADC` to „add with carry", czyli dodaj z przeniesieniem. Prawdziwy procesor zapisuje przy
skoku odległość w bajtach, a nie adres; nasz wprost.

## Cały procesor w jednej funkcji

Rejestry leżą w **strukturze**, czyli w pudełku z nazwanymi przegródkami:

```c
typedef struct {
    uint8_t  a;                  /* tu ląduje wynik arytmetyki */
    uint8_t  x;                  /* licznik */
    uint16_t pc;                 /* adres następnego rozkazu */
} cpu_t;
```

`uint8_t` to jeden bajt, czyli liczba od 0 do 255; `uint16_t` to dwa bajty, czyli adres.
`typedef` nadaje pudełku nazwę, żeby dalej pisać krótko `cpu_t`. To nie całe pudełko: leżą
w nim jeszcze `sp`, dwie flagi, stos i licznik wykonanych rozkazów. Wypisałem tylko te trzy
przegródki, bez których nie da się czytać rozkazów.

Nowy jest tu także `switch`: zamiast dwunastu `if`-ów wybiera jedną z gałęzi, a każda z nich
zmienia jeden albo dwa rejestry. Cały procesor to ten `switch`; funkcja `fetch` podaje bajty.

`DEX` zmniejsza `X` i zostawia po sobie **flagę**: bit, który mówi, czy wynik jest zerem. Skok
`BNE` nie porównuje liczb, tylko pyta flagę `Z`. Druga flaga, `C`, pamięta przeniesienie
z dodawania: kiedy dwie liczby jednobajtowe dadzą razem więcej niż 255, dziewiąty bit nie mieści
się w rejestrze i wpada do tej flagi. Tak procesor dodaje liczby większe niż jeden bajt, kawałek
po kawałku. Obie flagi widać w wyniku pod nagłówkami `Z=` i `C=`.

## Stos, czyli skąd procesor wie, gdzie ma wrócić

Stos to kupa bajtów z jedną zasadą: dokładasz na wierzch i zdejmujesz z wierzchu, nigdy
ze środka. **Podprogram** to kawałek programu, do którego się skacze i z którego trzeba
wrócić; w C tę samą rolę gra funkcja. Kiedy procesor skacze do podprogramu rozkazem `JSR`,
musi gdzieś zapisać, dokąd ma wrócić, więc odkłada na stos dwa bajty adresu. `RTS` zdejmuje
je i skacze pod odczytany adres. Podprogram nie musi wiedzieć, kto go zawołał. Zmienna `sp`
to numer wolnego miejsca, a stos rośnie w dół, więc każdy `JSR` zabiera dwa miejsca, a `RTS`
je oddaje. Przy starcie stos jest pusty i `sp` pokazuje ostatnie wolne miejsce, pod numerem
255.

## Jak to uruchomić

Ten etap jest programem na komputer, nie na płytkę, więc nie ma tu czego wgrywać. Z katalogu
`tutorial-pl` zbuduje go i uruchomi jedna komenda:

```bash
make STAGE=05 run
```

Wspólne budowanie etapów rozpoznaje, że to program na komputer, i sięga po kompilator
z twojego systemu, a nie po ten do procesorów Arm. Sprawdzenie, że kod nie ma ani jednego
ostrzeżenia, to `make STAGE=05 check`.

To samo bez `make`, z katalogu `etap-05`:

```bash
cc -Wall -Wextra -o etap05 main.c
./etap05
```

Sprawdzenie bez tworzenia pliku to `cc -Wall -Wextra -fsyntax-only main.c`.

## Co powinieneś zobaczyć

Trzy programy, każdy z tytułem, linią na każdy rozkaz i linią podsumowania. Każda linia z `A=`,
`X=` i `SP=` pokazuje stan rejestrów **przed** rozkazem z tej samej linii, więc skutek rozkazu
widać w linii następnej. Pierwszy program dodaje siedem sześć razy: `A` rośnie 7, 14, 21 i tak
dalej, aż po szóstym dodaniu pokazuje 42. Kiedy `X` dochodzi do zera, flaga `Z` zmienia się na 1.

Drugi program jest krótki i cały wart przeczytania, bo widać w nim stos:

```
0000  LDA   A=  0 X=  0  Z=0 C=0  SP=255
0002  JSR   A=  5 X=  0  Z=0 C=0  SP=255
0006  ADC   A=  5 X=  0  Z=0 C=0  SP=253
0008  RTS   A= 10 X=  0  Z=0 C=0  SP=253
0005  HLT   A= 10 X=  0  Z=0 C=0  SP=255
5 instructions, A = 10, X = 0, SP = 255 -- expected A = 10: ok
```

`JSR` odkłada adres powrotu i `sp` spada z 255 na 253, bo adres to dwa bajty. `RTS` go zdejmuje,
`sp` wraca na 255, a `pc` ląduje pod 0005. `ok` znaczy, że `A` zgadza się z oczekiwaniem,
a `wrong`, że nie.

## Ćwiczenia

Zmień w pierwszym programie liczbę dodań z 6 na 8 i popraw w `main` oczekiwanie z 42 na 56.
Program ma policzyć siedem razy osiem, czyli 56, a test powie `wrong`, dopóki nie zmienisz także
oczekiwania. Po każdej zmianie zbuduj i uruchom program ponownie.

Wstaw `OP_NOP` przed rozkazem `JMP` w trzecim programie. Adresy poniżej przesuną się o jeden,
ale `JMP` dalej prowadzi pod 0007, gdzie nie ma już `HLT`, tylko drugi bajt rozkazu `LDA #9`.
Maszyna czyta ten bajt jako rozkaz, a numery rozkazów nie mają z góry narzuconego porządku:
pod dziewiątką siedzi `BEQ`, czyli skok warunkowy. Ten skok potrzebuje dwóch bajtów adresu,
a za programem nie ma już nic, więc procesor wypada poza jego koniec. Uruchom program
i przeczytaj komunikat. To dokładnie ten rodzaj błędu, przed którym chroni asembler.

Dopisz na końcu funkcji `run` wypisanie dwóch ostatnich miejsc stosu, `cpu.stack[STACK_TOP]`
i `cpu.stack[STACK_TOP - 1]`, funkcją `printf`, tak jak robi to reszta pliku. Po drugim programie
leżą tam dwa bajty odłożone przez `JSR`: `00` pod `STACK_TOP` i `05` pod `STACK_TOP - 1`.
Młodszy bajt adresu leży na wierzchu, dlatego `RTS` zdejmuje go pierwszy.

Dopisz czwarty program: `LDX #5`, `LDA #0`, a w pętli `ADC #5` i `DEX`, aż `X` spadnie do zera.
Ma wyjść pięć razy pięć, czyli 25, więc pamiętaj o przeliczeniu adresu w skoku.

## Co dalej

Nasz procesor umie liczyć tylko na tym, co ma w rejestrach. W etapie 06 dostanie pamięć
i autobus: rozkazy zaczną podawać adresy, a procesor będzie pod nie zaglądał.
