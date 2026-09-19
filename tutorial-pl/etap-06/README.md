# Etap 06: pamięć i autobus, czyli skąd procesor bierze dane

W etapie 05 procesor miał rejestry i rozkazy, ale wszystkie liczby brał z powietrza:
podawałeś je w kodzie testu. Prawdziwy procesor nie nosi programu w sobie. Sięga po każdy
rozkaz i po każdą liczbę pod adres, a droga, którą to robi, nazywa się **autobusem**.
Ten etap go buduje.

Procesor jest tu mniejszy niż w etapie 05: został jeden rejestr `A` i pięć rozkazów, bo cała
uwaga idzie na drogę po dane, a nie na same rozkazy.

## Adres, czyli numer komórki

Pamięć to długi rząd bajtów, a każdy bajt ma numer. Ten numer to adres. Zapisujemy go
szesnastkowo, na przykład `0x0020`, bo jedna cyfra szesnastkowa opisuje cztery bity, a dwie
takie cyfry to cały bajt. Adres `0x0020` i liczba 32 to ta sama wartość, zapisana inaczej.
Rozkaz zajmuje tu dwa bajty: pierwszy mówi, co zrobić, drugi mówi, pod jakim adresem. Nawet
liczba podana przy rozkazie jest adresem: `LDA 0x20` znaczy „weź bajt spod 0x0020", a nie
„weź dwadzieścia".

## Autobus, czyli dwie funkcje między procesorem a światem

Reszta etapu opiera się na jednym pomyśle: procesor nie dotyka tablicy pamięci
bezpośrednio. Woła `mem_read` albo `mem_write`, a te dwie funkcje decydują, kto odpowie.
To jest autobus i ma jedną zaletę, którą pokazuje trzecie demo: **procesor nie musi
wiedzieć, co siedzi pod adresem**. Sprawdzanie adresu leży w jednym miejscu, więc program,
który poprosi o `0x9000`, dostanie zero i komunikat, a nie koniec świata.

## Mapa pamięci, czyli kto mieszka gdzie

Skoro wszystko ma adres, trzeba się umówić, co gdzie leży. Ten układ nazywa się mapą
pamięci i w tym etapie wygląda tak:

| Adres | Co tam jest |
|---|---|
| `0x0000` | program, który się właśnie wykonuje |
| `0x0010` | dwa bajty z adresem, pod który program zagląda |
| `0x0020` | liczba, na której pracuje pierwszy program |
| `0x0030` | miejsce, w którym drugi program zostawia wynik |
| `0x0080` | licznik, rejestr sprzętowy |
| `0x0081` | wynik, rejestr sprzętowy |
| `0x7FFF` | tu kończy się pamięć |

Dwa adresy, `0x0080` i `0x0081`, nie są pamięcią. Obsługują je te same dwie funkcje, ale
zamiast sięgać do tablicy, zmieniają licznik albo zapisują wynik. Nic tego nie pilnuje:
program, który wyjdzie poza swoje dane, zje własny kod. Mapa to umowa, nie bezpiecznik.

## Program, który zmienia sam siebie

Pierwszy program czyta jedną liczbę dwa razy i zmienia ją między odczytami:

    0x0000  LDA  0x20     A = bajt spod 0x0020, czyli 5
    0x0002  INC  0x20     bajt spod 0x0020 = 6
    0x0004  ADC  0x20     A = A + bajt spod 0x0020, czyli 5 + 6
    0x0006  STA  0x81     rejestr wyniku = A
    0x0008  HLT  0x00     stop; HLT nie patrzy na swój argument

Suma wychodzi 11, a nie 10, choć oba odczyty sięgają pod ten sam adres. Powód jest jeden:
`INC` zmienił bajt, który `ADC` czyta. Rozkazy leżą w tej samej pamięci co dane, więc `INC`
nie ma pojęcia, czy zmienia zmienną, czy kawałek programu.

## Pamięć kontra rejestr sprzętowy

Drugi program czyta ten sam adres dwa razy i za każdym razem dostaje inną liczbę:

    LDA  0xFF     A = bajt spod adresu, który siedzi w 0x0010
    ADC  0xFF     A = A + ten sam bajt jeszcze raz
    STA  0x30     bajt spod 0x0030 = A
    HLT  0x00     stop

`0xFF` to umówiony znak, że argumentem nie jest adres, tylko adres schowany w pamięci pod
`0x0010`. Argument rozkazu ma jeden bajt, więc zmieściłby się w nim każdy adres od `0x00`
do `0xFF`. Umawiamy się, że ostatnia wartość nie jest adresem, tylko odsyłaczem, i adres
`0x00FF` zostaje poza zasięgiem zwykłych rozkazów. Dzięki temu ten sam program można
skierować w dwa miejsca, nie zmieniając w nim ani jednego bajtu.

W drugim demo `0x0010` wskazuje na `0x0020`, gdzie leży siódemka. Oba odczyty dają 7, więc
suma wynosi 14. W trzecim demo `0x0010` wskazuje na licznik. Ten odpowiada najpierw 3,
a potem 2, więc suma wynosi 5. Adres był ten sam, a odpowiedź inna, bo pod spodem siedzi
układ. Pamięć oddaje to, co do niej włożyłeś, a rejestr to, co mówi sprzęt. Zapis do
takiego rejestru bywa rozkazem dla układu: „wyślij obraz", „przewiń ekran".

## Jak to uruchomić

To program na komputer, nie na płytkę: procesor i pamięć z tego etapu to model, a nie układy
na biurku. Panel wraca w etapie 08. Buduje się go tak:

```bash
cc -Wall -Wextra -o etap06 main.c
./etap06
```

Sprawdzenie, że kod nie ma ani jednego ostrzeżenia, to to samo polecenie bez tworzenia pliku:

```bash
cc -Wall -Wextra -fsyntax-only main.c
```

Komenda `make STAGE=06 check` z katalogu `tutorial-pl` nie zadziała i to jest w porządku:
ten Makefile buduje etapy dla mikrokontrolera i dokłada `startup_l476.s` oraz `linker.ld`,
które dla programu na komputer nie mają sensu. Kompilacja etapu przechodzi bez ani jednego
ostrzeżenia, ale gotowego programu nie da się złożyć, bo `printf` wypisuje tekst przez system,
którego na płytce nie ma.

## Co powinieneś zobaczyć

Program wypisuje trzy demo, jedno po drugim, i mówi po angielsku, tak jak kod.
`A after the run` to wartość rejestru `A` po zakończeniu programu, `result register`
to rejestr wyniku, `instructions run` to liczba wykonanych rozkazów, a `program halted`
mówi, czy program się zatrzymał.

W pierwszym demo zrzut pamięci przed uruchomieniem i po nim różni się jednym bajtem: pod
`0x0020` jest 6 zamiast 5, a reszta programu stoi w miejscu. Suma to 11. W drugim demo oba
odczyty dają 7 i suma to 14. W trzecim licznik odpowiada 3 i 2, a suma to 5.

Zrzut, czyli zawartość pamięci bajt po bajcie, wygląda tak:

```
  0000: 01 20 04 20 03 20 02 81 00 00 00 00 00 00 00 00
  0010: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0020: 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

Czytaj go tak jak w debuggerze, czyli w narzędziu, które pokazuje pamięć działającego
programu: `0000:` to adres pierwszego bajtu w wierszu, a dalej szesnaście liczb. Pierwszy
wiersz to cały program: dziesięć bajtów, a za nimi zera. W trzecim wierszu leży liczba,
na której pracuje program: 05 przed uruchomieniem, a 06 po nim.

## Ćwiczenia

Zmień w pierwszym programie wartość początkową `mem[DATA1]` z 5 na 100. Ile teraz wynosi
suma i dlaczego?

Dopisz do drugiego programu trzeci odczyt tego samego adresu. W demo 3 licznik odpowiada
wtedy 3, 2 i 1. Zastanów się, jak zmienia się suma. Ten sam program liczy też demo 2, więc
tam suma również się zmieni.

Skieruj program na adres, którego nie ma, dopisując w demo 3 `point_at(0x9000)`. Komunikat
pojawi się dwa razy, bo program zagląda pod ten adres przy każdym z dwóch odczytów.
Przeczytaj go i powiedz, co się stało z odczytem. To samo sprawdzenie chroni emulator przed
programem, który szuka nieistniejącego układu.

Dodaj do autobusu drugi licznik pod adresem `0x0082`, który przy pierwszym odczycie daje 10.
Policz, ile miejsc trzeba zmienić: zmienną na stan licznika, jedną gałąź w `mem_read` i koniec
zakresu w mapie.

## Co dalej

W etapie 07 dostaniesz prawdziwy kartridż do ręki. Dowiesz się, że gra to plik z nagłówkiem
i dwiema częściami, programem i grafiką, i wypiszesz jego pola na ekranie.
