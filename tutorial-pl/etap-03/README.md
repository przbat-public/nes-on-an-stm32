# Etap 03: czas i zegar, czyli obraz, który zmienia się sam

Poprzednie etapy kończyły się tak samo: program rysował jeden obraz i nic więcej się nie
działo, a każda zmiana wymagała wgrania programu od nowa. Ten etap to zmienia: panel
wreszcie pokaże ruch. Zrobimy przy okazji dwie rzeczy, które spotkasz w prawie każdym
programie na mikrokontroler. Podniesiemy taktowanie, czyli liczbę uderzeń zegara na sekundę.
I sprawimy, że program przestanie stać w miejscu: będzie rysował obraz za obrazem.

## Po co układowi zegar

Mikrokontroler nie robi nic „od razu". Ma w środku zegar, który wystukuje równe uderzenia,
a każde uderzenie pozwala wykonać jeden krok pracy: pobrać rozkaz, dodać dwie liczby, wysłać
bajt. Jedno uderzenie to jeden **takt**. Liczbę taktów na sekundę zapisujemy w megahercach,
w skrócie MHz: jeden megaherc to milion uderzeń na sekundę.

Po włączeniu zasilania płytka używa zegara startowego o częstotliwości około 4 MHz. Ten zegar
siedzi w środku układu, więc działa od razu, bez dodatkowych elementów na płytce. Ma też
wadę, którą widziałeś w etapie 00, kiedy ekran wypełniał się zauważalnie długo.

## Rejestr, czyli tablica przełączników

Rejestr to miejsce w układzie, które ma swój adres i 32 przełączniki. Każdy przełącznik
nazywamy **bitem** i numerujemy od zera: bit numer 0 to pierwszy przełącznik, bit numer 1
to drugi. Osiem bitów to jeden bajt, więc rejestr ma cztery bajty. Kiedy piszesz do rejestru
liczbę, ustawiasz te przełączniki. Który z nich za co odpowiada, mówi dokumentacja układu.

Dlatego w kodzie spotkasz zapis `(1u << 8)`: to jedynka przesunięta o osiem miejsc w lewo,
czyli bit numer osiem. Literkę `u` przy jedynce na razie pomiń; to informacja dla kompilatora.
Każdy przełącznik ma swoją wagę: pierwszy znaczy 1, drugi 2, trzeci 4, a każdy następny dwa
razy tyle. Bit numer osiem znaczy więc 256 i tyle samo ustawiłaby wpisana wprost liczba `256`.
Zapis `(1u << 8)` mówi jednak od razu, o który przełącznik chodzi.

Drugi znak, który zobaczysz w kodzie, to `&`: między dwiema liczbami zostawia tylko te bity,
które są włączone w obu. Zapis `SPI1_SR & (1u << 1)` pyta więc, czy bit numer jeden jest
teraz włączony.

## Skąd wziąć osiemdziesiąt milionów taktów

Zegara startowego nie da się przyspieszyć, ale układ ma w środku drugie źródło taktu, zwane
oscylatorem. Bije ono 16 milionów razy na sekundę (w dokumentacji nosi nazwę HSI16). To
źródło włączamy i podajemy jego wyjście do układu o nazwie **PLL**, czyli pętli
synchronizacji fazy. Nazwa obca, pomysł prosty: to układ mnożący z dzielnikiem.

Trzy liczby (dzielnik, mnożnik i drugi dzielnik) siedzą w jednym rejestrze `RCC_PLLCFGR`,
każda w swojej grupie bitów. Obok nich dwa bity wybierają, skąd PLL bierze zegar. Nasza
arytmetyka jest krótka: pierwszy dzielnik zostaje w spokoju, mnożnik ustawiamy na 10, drugi
dzielnik na 2. Szesnaście milionów razy dziesięć daje sto sześćdziesiąt milionów, a połowa
z tego to osiemdziesiąt.

Te pola nie trzymają liczb, które wpisałbyś na kartce. Pole pierwszego dzielnika przechowuje
wartość o jeden mniejszą, pole drugiego dzielnika połowę wartości minus jeden, a pole
mnożnika samą liczbę. Osobny bit włącza jeszcze wyjście PLL: bez niego PLL liczy, ale nic
z tego nie wychodzi. Kto o tym zapomni, dostanie nie tę częstotliwość, którą zamówił.

Kolejność jest sztywna i wynika z tego, jak działa sprzęt. Najpierw mówimy pamięci, ile
taktów ma na odpowiedź; dlaczego, wyjaśnia następna sekcja. Potem uruchamiamy wewnętrzny
zegar 16 MHz i czekamy, aż się ustabilizuje. Następnie włączamy PLL i czekamy, aż złapie
synchronizację, bo przez pierwsze takty jego częstotliwość pływa. Na końcu przełączamy
procesor na wyjście PLL i sprawdzamy, czy przełączenie się udało. Trzy z tych kroków kończą
się pętlą `while`, czyli pętlą, która powtarza się, dopóki warunek jest prawdziwy. Sprzęt
nie zmienia się w tej samej chwili, w której go o to prosisz.

## Dlaczego przy okazji trzeba zwolnić pamięć

Nanosekunda to miliardowa część sekundy, więc jeden takt przy 80 MHz trwa 12,5 nanosekundy.
Pamięć flash, w której leży program, nie odpowiada tak szybko. Procesor czekałby na każdy
rozkaz, więc mówimy pamięci, ile taktów ma na odpowiedź: to **stany oczekiwania**. Cztery
stany dodają cztery takty do każdego odczytu i to wystarcza. Trzy bity obok włączają pamięci
podręczne, czyli małe, szybkie schowki w środku procesora, żeby rozkazy nie musiały wracać
do pamięci.

## Pętla, która się nie kończy

Program na komputerze oddaje sterowanie systemowi operacyjnemu, kiedy dobiegnie końca.
Tutaj nie ma komu oddać: pod adresem, do którego wróciłby `main`, nie ma nic. Dlatego `main`
kończy się pętlą `for (;;)`, którą widziałeś już w poprzednich etapach. Dotąd stała pusta
i tylko trzymała program przy życiu. Tę pętlę nazywamy główną, bo teraz siedzi w niej cała
animacja:

```c
uint32_t frame = 0;

for (;;) {
    int bar_x = (int)((frame * PIXELS_PER_FRAME) % PANEL_W);
    draw_frame(bar_x);
    push_framebuffer();
    delay_ms(20);
    frame++;
}
```

`draw_frame` robi to, co `draw_scene` w poprzednim etapie, tylko zamiast stałego obrazu
rysuje pasek na wskazanej pozycji. Najpierw zamalowuje cały bufor na czarno, a potem stawia
w nim pasek. `push_framebuffer` wysyła gotowy bufor na panel, a `delay_ms` czeka.

`uint32_t` to liczba mieszcząca się w czterech bajtach; `uint16_t` z etapu 01 mieścił się
w dwóch. Licznik `frame` rośnie o jeden na każdy obieg, a pozycja paska wynika z tego
licznika. Stała `PIXELS_PER_FRAME` mówi, że pasek przesuwa się o cztery piksele na klatkę,
czyli na jedno pełne odświeżenie obrazu. Reszta z dzielenia przez `PANEL_W`, czyli przez
320, sprawia, że po dojściu do prawej krawędzi pasek pojawia się znowu po lewej. Pasek ma
24 piksele szerokości (stała `BAR_W`), więc tuż przed zawinięciem widać tylko jego koniec.
Pozycję liczymy z numeru klatki, żeby błąd nie zbierał się z klatki na klatkę. Zapis `(int)`
przed nawiasem mówi kompilatorowi, że wynik ma być zwykłą liczbą całkowitą, taką jak `bands`
w etapie 01; na razie możesz go pomijać.

## Dlaczego nie da się mierzyć czasu obrotami pętli

Kusi, żeby poczekać, wykonując sto tysięcy pustych obrotów pętli. To działa, dopóki nic się
nie zmieni: każda poprawka w kodzie zmienia czas jednego obrotu, więc sto tysięcy obrotów
przestaje być tą samą chwilą.

Dlatego czekanie dostaje nazwę i jednostkę. Funkcja `delay_ms` bierze liczbę milisekund,
czyli tysięcznych części sekundy, i opiera się na jednej policzonej liczbie: stała
`LOOPS_PER_MS` mówi, ile obrotów wewnętrznej pętli przypada na jedną milisekundę przy
80 MHz. Wartość wyszła z policzenia rozkazów, na które kompilator zamienia tę pętlę,
a komentarz w kodzie mówi wprost, że to przybliżenie. Dokładnie mierzy czas tylko układ,
który sam liczy takty zegara; taki układ pojawi się w etapie 10.

## Co powinieneś zobaczyć

Czerwony pasek przesuwający się po czarnym tle od lewej krawędzi do prawej i wracający
na początek. W prawym dolnym rogu stoi zielony kwadrat i nie rusza się ani o piksel: zdradzi
obrócenie albo odbicie obrazu, gdyby coś takiego się stało.

Obraz ma 76 800 pikseli, a każdy jedzie do panelu jako dwa bajty, czyli szesnaście bitów.
Szyna, czyli droga, po której bajty jadą do panelu, pracuje z połową taktu procesora, czyli
40 MHz. Sama transmisja zajmuje więc ponad trzydzieści milisekund i dlatego pasek skacze,
a nie sunie.

## Zbuduj i wgraj

Otwórz terminal w katalogu `tutorial-pl` i zbuduj kod jednym poleceniem:

```bash
make STAGE=03 check
```

`check` tylko kompiluje i nic nie wgrywa. Włącza przy tym ostrzeżenia (`-Wall -Wextra`), więc
od razu widać każde podejrzane miejsce w kodzie. Razem z `main.c` kompilują się pożyczone
z emulatora `../src/startup_l476.s` i `../src/linker.ld`.

Żeby zobaczyć pasek, wgraj etap na płytkę:

```bash
make STAGE=03 flash
```

Sam kod waży półtora kilobajta, a bufor obrazu zajmuje 76 800 bajtów z 96 kilobajtów pamięci.

## Ćwiczenia

Zmień dwadzieścia milisekund w pętli głównej na dwieście, a potem na zero. Przy dwustu pasek
będzie się wlókł, przy zerze pomknie tak szybko, jak zdąży go narysować i wysłać program.

Odwróć kierunek ruchu. Zmiana znaku w liczeniu pozycji nie wystarczy, bo reszta z dzielenia
liczby ujemnej wychodzi ujemna. Zrób to warunkiem, który zmienia kierunek po dojściu
do krawędzi, i spraw, żeby pasek odbijał się tam i z powrotem.

Narysuj dwa paski naraz, czerwony i żółty, i przesuń je w przeciwne strony. Pamiętaj, że
`draw_frame` zaczyna od zamalowania całego bufora, więc oba paski muszą powstać w tej samej
klatce, zanim obraz wyruszy na panel.

Zamień pasek na licznik sekund. Trzymaj osobną zmienną `uptime_ms`, dodawaj do niej
dwadzieścia przy każdej klatce i maluj na dole ekranu pasek rosnący o jeden piksel
na sekundę. Zobaczysz wtedy, jak bardzo `delay_ms` rozmija się z prawdziwym czasem.

## Co dalej

Cała klatka jedzie do panelu ponad trzydzieści milisekund, a procesor przez ten czas czeka.
W etapie 04 podzielimy obraz na pasma i wyślemy je po kolei. Procesor dostanie wtedy czas,
którego teraz nie ma: gdy jedno pasmo jedzie po szynie, może liczyć następne.
