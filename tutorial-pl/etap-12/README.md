# Etap 12: przewijanie obrazu i podział ekranu

W etapie 11 ruszałeś duszkami, ale świat pod nimi stał w miejscu: każdy obraz, jaki dotąd
powstał, miał piksele tam, gdzie je narysowano. Gra, w której idziesz w prawo bez końca,
nie może tak działać. Potrzebowałaby obrazu całego poziomu, a na to konsola nie ma pamięci.

Konsola rozwiązuje to jednym rejestrem. Nic w obrazie się nie przesuwa: rejestr mówi, od
którego miejsca świata zaczyna się obraz, więc wartość o jeden większa przesuwa cały widok
o piksel. Dzięki temu jeden ekran pokazuje dwie rzeczy naraz: pasek statusu, który stoi,
i planszę, która jedzie pod nim.

## Rejestr przewijania

Poprzednie etapy liczyły dla każdego piksela jego miejsce w tablicy kafli. Teraz do tego
liczenia dochodzi jedna liczba:

```c
int wx = (scroll_x + px) % SCREEN_W;
int wy = (scroll_y + py) % (WORLD_ROWS * TILE);
```

`px` i `py` to kolumna i wiersz piksela na ekranie, a `scroll_x` i `scroll_y` to pozycja okna
w świecie. `SCREEN_W` to szerokość ekranu, 256 pikseli z etapu 08. `WORLD_ROWS * TILE` to
wysokość świata w pikselach: 56 kafli po 8 pikseli, czyli 448.

Piksel w kolumnie `px` pokazuje to, co w świecie leży `scroll_x` pikseli dalej. Gdy
`scroll_x` rośnie o jeden, cały obraz przesuwa się o piksel, a ty nie przerysowałeś ani
jednego kafla. Dzielenie z resztą sprawia, że świat się zawija: gdy okno dojdzie do
krawędzi, po prawej pokaże się to, co było po lewej. Konsola robi tak samo, dlatego poziom
może być krótszy niż droga, którą przechodzisz. Rejestr pionowy działa jak poziomy, więc
jedna para liczb obsługuje oba kierunki.

## Kiedy wolno zmienić rejestr

Ekran powstaje wiersz po wierszu, od góry do dołu, a rejestr jest jeden. Zmieniony w połowie
obrazu zostawia to, co już narysowane, w starym układzie, a resztę rysuje w nowym. Moment,
w którym wolno go zmienić, nazywa się **horizontal blank**: to krótka przerwa między dwoma
wierszami obrazu, kiedy układ obrazu nie rysuje. Zmiana zrobiona w tej przerwie obowiązuje
od następnego wiersza. Pilnuje tego funkcja `ppu_end_row`: po narysowaniu wiersza kopiuje
rejestry do tego, czym będzie rysował następny. Wartości zapisane w trakcie wiersza czekają
więc do przerwy, zamiast działać natychmiast.

## Skąd program wie, że to już czas

Program nie liczy wierszy sam: w tym samym czasie czyta przyciski, przesuwa duszki i odmierza
czas. Konsola daje mu do tego jedną flagę. Jednym z duszków jest **duszek numer zero**,
pierwszy na liście, i ten duszek jest znacznikiem podziału. Układ obrazu sprawdza podczas
rysowania, czy piksel znacznika trafił na piksel tła inny niż niebo, i jeśli tak, zapala bit.
Program czeka na ten bit w funkcji `cpu_wait_for_sprite`: jej pętla rysuje wiersz po wierszu,
aż bit się zapali.

Znacznik stoi tak, że jego górny wiersz wypada w ostatnim wierszu paska statusu, a pod nim
leżą dwa kafle cegły, które pasek zasłania. Bez nich znacznik stałby nad samym niebem i bit
nigdy by się nie zapalił. Dzięki nim bit zapala się dokładnie wtedy, gdy układ obrazu kończy
pasek, i wtedy program pisze rejestr przewijania.

## Jak to wygląda w kodzie

Świat to tablica kafli, 32 kolumny i 56 wierszy: grunt, schody, półka z cegieł i drzewa.
Pasek statusu zajmuje górnych 16 wierszy ekranu (`SPLIT_ROW`), czyli dwa kafle, a wszystko
poniżej przewija się razem ze światem.

Układ obrazu to struktura `ppu_t` (od PPU, układu obrazu z etapu 08): rejestry przewijania,
kopia tego, czym rysuje się bieżący wiersz, flaga duszka numer zero i bufor obrazu. Funkcja
`ppu_render_row` rysuje jeden wiersz, najpierw tło, potem duszki na wierzchu. Pasek rysuje
`draw_status_bar`: ciemne tło, napisy `SCORE` i `SCROLL` z liczbami, pasek postępu i biała
linia pod spodem. Litery powstają piksel po pikselu z małej tablicy wzorów, tak jak kafle,
bo panel nie ma pojęcia, czym jest litera.

## Co powinieneś zobaczyć

```bash
cc -Wall -Wextra -o etap12 main.c -lz
./etap12
```

`-lz` dołącza bibliotekę `zlib`: ona pakuje obraz do pliku PNG i poza tym zapisem nie ma
z programem nic wspólnego.

Powstanie plik `etap12.png`. Otwórz go i popatrz na dwie rzeczy naraz. Pasek statusu stoi:
`SCORE` z liczbą, pod nim `SCROLL` z liczbą, pasek postępu żółty od lewej i biały dalej,
a na końcu biała linia. Plansza pod nimi jedzie: drzewa, schody i półka z cegieł przesunęły
się o czterdzieści pikseli w lewo i w górę. Tyle wychodzi z czterdziestu klatek, w których
program przesuwał świat o piksel na klatkę (`SCROLL_PER_FRAME`). Znacznik siedzi tuż pod
paskiem i stoi w miejscu: przewijanie jego pozycji nie dotyczy.

Program wypisze linię `frame 40, score 280, scroll 40 pixels, marker fired at row 16`,
a pod nią wynik sześciu sprawdzeń. Szóste z nich opisuje celowy błąd: rejestr zapisany,
zanim pasek powstał. Powstaje wtedy drugi plik, `etap12-niecierpliwy.png`. Otwórz go obok
pierwszego: paska nie ma, a na jego miejscu widać świat przewinięty tak samo jak resztę
obrazu. Jeden pośpiech i z paska statusu robi się druga kopia świata.

## Ćwiczenia

Zmień `SCROLL_PER_FRAME` na 8. Plansza jedzie szybciej, a pasek stoi tam, gdzie stał. Po
kilkudziesięciu klatkach pozycja przekracza `SCROLL_LIMIT`, czyli największe przewinięcie,
przy którym widać jeszcze pasmo świata, i program zawija ją do zera. Obraz przeskakuje.
To ten moment, w którym gra musi doładować nowy fragment świata.

Przesuń znacznik o osiem pikseli w górę, zmieniając `SPRITE_Y` z `SPLIT_ROW - 1` na
`SPLIT_ROW - 9`. Bit zapali się wcześniej, więc program zapisze rejestr, zanim układ obrazu
skończy pasek. Dolna część paska zniknie pod planszą, tak jak w pliku
`etap12-niecierpliwy.png`, tylko z innej przyczyny. W drugą stronę, na `SPLIT_ROW + 7`,
znacznik schodzi z kafli, bit nie zapala się ani razu i plansza stoi w miejscu. Program
wypisze wtedy `2 check(s) failed`: brak bitu wywraca sprawdzenie samego bitu i sprawdzenie
ruchu planszy.

Zmień w pętli głównej `scroll_write(&ppu, scroll, scroll)` na
`scroll_write(&ppu, scroll, 224)`. Liczba 224 to wysokość widocznej części świata: 240
wierszy ekranu minus 16 wierszy paska. Okno dojedzie wtedy do końca pasma i od dołu pokaże
się jego początek: pas nieba i dwa kafle cegły, na których stoi znacznik. Tak działa reszta
z dzielenia: świat nie kończy się na krawędzi, tylko wraca z drugiej strony.

Zamień w `tile_palette` kolor trawy (`C_LIME`) na kolor nieba (`C_SKY`). Przewijanie działa
dalej, ale obraz przestaje być czytelny. Pomyśl, dlaczego ruch widać tylko tam, gdzie
stykają się dwa różne kolory.

Dodaj do paska numer klatki. Funkcja `draw_status_bar` go nie zna, więc dopisz jej parametr,
a w pętli głównej podaj `frames`. Liczbę wypisujesz cyfra po cyfrze, tak jak liczbę punktów,
i służy do tego `format_number`: obraz to piksele, nie tekst, więc nie ma gdzie wpisać
liczby.

## Sprawdzenie

Ten etap jest programem na komputer, nie na płytkę, więc wspólne `make STAGE=12 check` go
nie zbuduje: wspólny Makefile sięga po kompilator dla procesorów Arm i po plik startowy
płytki, a ten program potrzebuje biblioteki `zlib`, której w zestawie dla Arm nie ma.
Komenda kończy się na `fatal error: zlib.h: No such file or directory`. Sprawdzenie wygląda
więc tak:

```bash
cc -Wall -Wextra -o etap12 main.c -lz && ./etap12
```

Kompilator nie wypisuje ani jednego ostrzeżenia, a program kończy się kodem zera i wypisuje
`0 check(s) failed`. Sześć sprawdzeń to: brak bitu przed wierszem znacznika, bit po jego
narysowaniu, ruch planszy o piksel, nieruchomy lewy górny róg paska, zgodność przesunięcia
o kafel z odczytem świata o kafel dalej, i na końcu sprawdzenie błędu pośpiechu.

## Co dalej

Świat w tym etapie zawija się w kółko. Prawdziwy poziom jest dłuższy niż pamięć konsoli,
więc gra podmienia jego fragmenty w trakcie działania. W etapie 13 zobaczysz, jak kartridż
decyduje, co leży pod danym adresem.
