# Etap 15: dlaczego to chodzi wolno, czyli pomiar zamiast zgadywania

Gra z etapu 14 działa, ale chodzi wolno. Odruch podpowiada zgadywanie: pewnie winna jest pętla
rysująca, pewnie tabela kolorów. Ten etap najpierw mierzy, a potem zmienia to, na co wskazał
pomiar. Mierzy na małej własnej grze, planszy z ptakiem, bo obraz ma tu być prosty do
sprawdzenia, a nie ciekawy. Efekt to ta sama klatka narysowana blisko cztery razy szybciej.

## Dwa sposoby mierzenia czasu

Pierwszy sposób siedzi w emulatorze: procesor liczy takty, które zużył, jak prawdziwy układ.
Liczba rozkazów na nic się nie zda, bo jeden rozkaz trwa dwa takty, a inny sześć, a program,
który czeka w pętli, też zjada klatkę. Procesor konsoli pracuje z zegarem 1,79 MHz, czyli
1,79 miliona taktów na sekundę, a obraz powstaje sześćdziesiąt razy na sekundę. Z grubsza
wychodzi z tego trzydzieści tysięcy taktów na klatkę; dokładna liczba, której trzyma się ten
program, to 29 780.

Drugi sposób jest na komputerze: `clock()` z biblioteki standardowej liczy czas procesora, który
zużył nasz program, więc dwa wywołania wokół jednej fazy klatki dają jej czas. Pomiar ma jednak
pułapki: komputer odmierza czas skokami, a inny program dokłada czas, który z emulatorem nie ma
nic wspólnego. Dlatego mierzymy sześć razy i zostawiamy najszybszy przebieg.

## Gdzie znika klatka

Klatka emulatora dzieli się na trzy fazy i każda ma w tabeli swój wiersz: procesor (wykonywanie
rozkazów programu z kartridża), rysowanie (zamiana pamięci układu obrazu, czyli tablicy kafli,
wzorów kafli i palety, na piksele) i panel (zamiana numerów kolorów na dwa bajty, które na
płytce pojechałyby drutem). Liczby w tabeli to milisekundy na klatkę. Pierwszy pomiar wygląda
tak:

```
milliseconds         as it was           +walk          +words  +changed bands
processor                0.092           0.092           0.091           0.089
drawing                  0.200           0.137           0.137           0.003
panel                    0.071           0.071           0.071           0.001
one frame                0.364           0.300           0.298           0.093
frames/second             2748            3331            3351           10785
```

Nazwy kolumn mówią, jaka zmiana jest w każdej z nich zmierzona: `as it was` („jak było”) to
rysowanie z etapu 13, `+walk` pierwsza zmiana, `+words` druga, czyli gotowe pary bajtów,
a `+changed bands` trzecia. Wiersz `one frame` sumuje trzy fazy, a `frames/second` mówi, ile
klatek na sekundę wyrobiłby sam komputer. Rysowanie kosztuje najwięcej, procesor i panel
zostają daleko za nim. Na płytce dochodzi do tego transmisja drutem i wolniejszy zegar, więc te
liczby nie przełożą się na nią jeden do jednego: ten etap mierzy pracę emulatora, a nie
przesyłanie obrazu.

## Pierwsza zmiana: chodzić, nie liczyć

Kod z etapu 13 liczy dla każdego piksela, którą kolumnę świata ten piksel pokazuje: dodaje
przewinięcie i dzieli z resztą przez 256, a potem dzieli wynik przez 8, żeby trafić w kafel.
To trzy dzielenia na piksel, prawie dwieście tysięcy na klatkę. Nowa wersja idzie, zamiast
liczyć: kolumna przesuwa się o jeden na piksel i sama wraca do zera po dwustu pięćdziesięciu
sześciu, a kafel zmienia się co osiem pikseli, więc zamiast reszty z dzielenia jest porównanie.
Prawie jedna trzecia pracy w fazie rysowania: 0,200 ms schodzi do 0,137 ms.

## Druga zmiana: gotowe słowa dla panelu

Panel dostaje dwa bajty na piksel, a kod buduje je za każdym razem od zera: bierze numer koloru,
wyciąga z tabeli kolor i rozkłada go na dwa bajty. Zamiast tego można trzymać tabelę kolorów już
rozłożonych na dwa bajty, w kolejności, w jakiej chce je panel. Brzmi jak oszczędność dwóch
przesunięć na piksel i nią jest. Pomiar mówi jednak coś innego: 0,071 ms przed zmianą, 0,071 ms
po niej, ani jednej tysięcznej. Okazuje się, że kosztuje nie arytmetyka, a pętla i dotykanie
pamięci.

## Trzecia zmiana: nie robić tego, co się nie zmieniło

Najwięcej dało nie przyspieszanie pętli, ale to, że w ogóle jej nie uruchamiamy. Obraz w tej
grze prawie stoi: ptak przesuwa się o jedno pole co czwartą klatkę, a reszta planszy się nie
rusza. Emulator rysował tymczasem wszystkie 61 440 pikseli od nowa, sześćdziesiąt razy na
sekundę, żeby pokazać to samo.

Układ obrazu wie, co się zmieniło, bo sam przyjmuje zapisy: każdy zapis do pamięci układu
obrazu, czyli do tablicy kafli, zaznacza swoje **pasmo**, czyli osiem wierszy pikseli. Zapis
palety, wzorów kafli albo rejestru przewijania zaznacza wszystkie pasma, a na końcu klatki
emulator flagi czyści.

Wiersz `+changed bands` pokazuje, ile to daje: rysowanie 0,003 ms zamiast 0,137, cała klatka
0,093 ms zamiast 0,364. Trzeba za to zapłacić dodatkową robotą: emulator musi przy każdym
zapisie zapamiętać, które pasmo się zmieniło. Przy przewijaniu oszczędność znika, bo wtedy
zmienia się każdy wiersz obrazu.

## Co powinieneś zobaczyć

```bash
cc -Wall -Wextra -o etap15 main.c && ./etap15
```

Program mierzy 180 klatek, a potem wypisuje tabelę z czterema kolumnami. Liczby będą inne niż
moje, bo zależą od komputera i od tego, co robi w tej chwili, ale układ zostanie: procesor
prawie tyle samo w każdej kolumnie, bo jego nikt nie ruszał, rysowanie maleje po pierwszej
zmianie, a po trzeciej spada do tysięcznych. Ile dokładnie maleje, zależy od kompilatora;
ćwiczenie z optymalizacją pokazuje, że pierwsza zmiana potrafi nie dać nic. Pod tabelą stoją
dwie liczby taktów, średnia na klatkę i ta, którą daje konsola (29 780), a do tego to, ile
z 16,667 ms, czyli jednej sześćdziesiątej sekundy, zużywa ostatnia wersja. Dalej idą dwa wiersze
sum kontrolnych, czyli liczb policzonych z zawartości obrazu: w każdym wierszu cztery identyczne
wartości, bo wszystkie wersje narysowały ten sam obraz i wysłały te same bajty. Na końcu program
wypisuje jeszcze ten obraz, jeden piksel na sześćdziesiąt cztery: `b` to niebieskie niebo i woda,
`w` białe chmury, wzgórza i ptak, `g` zielona trawa, a `y` żółte drzewa i mur.

## Sprawdzenie

Ten etap jest programem na komputer, nie na płytkę. Wspólny `Makefile` wie o tym i buduje go
kompilatorem systemowym, więc z katalogu `tutorial-pl` wystarczy:

```bash
make STAGE=15 check    # kompilacja, zero ostrzeżeń
```

`check` nic nie wgrywa i nic nie zapisuje, a to samo robi ręka z katalogu `etap-15`:
`cc -Wall -Wextra -o etap15 main.c`. Dopisanie `&& ./etap15` uruchamia program i pokazuje
tabelę.

Ten etap i tak nie zmieściłby się na płytce. Płytka ma 96 kilobajtów pamięci, a program trzyma
obraz, bufor bajtów dla panelu i cały kartridż, czyli grubo ponad dwieście kilobajtów;
kompilator dla procesorów Arm kończy pracę komunikatem `region 'RAM' overflowed by 125228 bytes`.
Do tego `printf` i `clock()` potrzebują systemu, którego płytka nie ma: `_write` i jego sąsiedzi
to funkcje, którymi program na komputerze pisze na ekran i czyta pliki.

## Ćwiczenia

Zbuduj program z optymalizacją: `cc -O2 -Wall -Wextra -o etap15 main.c && ./etap15`. Kompilator
sam zamienia dzielenie przez stałą na przesunięcia bitów i mnożenia, więc cała tabela się
zmienia: u mnie pierwsza kolumna zrobiła się szybsza od drugiej, a trzecia zmiana została jedyną,
która naprawdę się opłaca. To, co opłaca się bez optymalizacji, nie musi opłacać się z nią.

Ustaw `REPEATS` na 1 i uruchom program trzy razy, a potem wróć do sześciu: zobacz, jak bardzo
tabela rusza się między uruchomieniami i ile z tego szumu zostaje po sześciu próbach. Stała
`REPEATS` stoi w `main.c`, przy pomiarze.

Zmień `ppu_frame_drawn()` w `main.c` tak, żeby nic nie robiła. Ta funkcja czyści na końcu klatki
zaznaczone pasma, więc bez niej po pierwszej klatce każde pasmo zostanie zaznaczone na zawsze
i wersja z pomijaniem pasm zacznie rysować wszystko. Obraz się nie zmieni, zmieni się tylko
tabela.

Spraw, żeby gra przewijała obraz: w funkcji `run`, w pętli klatek, dopisz przed `render_picture`
dwie linie, `ppu_write(0x2005, frame)` i `ppu_write(0x2005, 0)`. To zapisy do rejestru
przewijania z etapu 12; w tym kodzie nosi on numer `0x2005`. Obraz pojedzie w bok, a ostatnia
kolumna straci całą przewagę.

## Co dalej

Po tych zmianach największą część klatki zajmuje procesor: 0,089 ms z 0,093. Emulator spędza ten
czas, wykonując rozkazy programu, który prawie całą klatkę czeka na sygnał układu obrazu. To
następne miejsce do przyjrzenia się; w repozytorium, w `src/`, leży gotowy emulator.
