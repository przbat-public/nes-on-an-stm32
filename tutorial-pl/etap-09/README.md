# Etap 09: duszki, czyli obraz, który się rusza

W etapie 08 obraz powstał z kafli wpisanych w siatkę: każdy kafel trafiał w całą komórkę
albo w żadną jej część. Bohater, który chce stanąć w połowie komórki, ani pocisk lecący
w połowie wiersza nie mieszczą się w takim obrazie.

## Duszek to obrazek z pozycją

Ten etap dodaje drugą połowę obrazu konsoli: **duszki**, czyli małe obrazki trzymane
osobno, z pozycją liczoną w pojedynczych pikselach. Konsola czyta je z osobnej tablicy
wzorów, a nie z tej, z której bierze kafle tła. Duszek nie jest wpisany w siatkę, więc
jego lewy górny róg może wypaść na dowolnym pikselu.

W kodzie etapu duszek to struktura `sprite_t`, czyli pudełko z pięcioma polami: `x`, `y`,
`pixels`, `w` i `h`. Dwa pierwsze to pozycja, `pixels` wskazuje tablicę pikseli, a `w` i `h`
to wymiary obrazka. Pozycja i obrazek siedzą w niej osobno i to jest cała różnica.
Żeby coś przesunąć, zmieniasz `x`, a nie zawartość tablicy: obrazek zostaje ten sam,
zmienia się tylko miejsce, w którym go kładziesz.

Konsola rysuje duszki o rozmiarze osiem na osiem albo osiem na szesnaście, a większa
postać to kilka takich kwadratów zszytych razem. Nasz bohater ma szesnaście na szesnaście,
czyli cztery kwadraty, a jego pozycję liczymy w pikselach, więc nie musi trafiać na granicę
kafla. Lista duszków ma trzy wpisy: bohatera, drzewo i robaka. Drzewo nigdy się nie rusza,
a mimo to jest duszkiem. Dzięki temu nie wpisujemy go w siatkę i możemy pokazać, jak jedno
zasłania drugie.

## Obraz powstaje od nowa przy każdej klatce

Nikt nie trzyma w pamięci obrazu z bohaterem w środku. Jest tło i lista duszków, a obraz
powstaje od nowa przy każdej klatce. Funkcja `draw_frame` robi trzy rzeczy: najpierw
`compose_background` zamienia siatkę kafli na piksele w całym buforze, potem
`compose_sprites` kładzie na tym duszki jeden po drugim, a na końcu `push_picture` wysyła
gotowy obraz na panel. O kolejności warstw decydują dwa pierwsze wywołania. Pozycja duszka
może wypaść za obraz; `compose_sprite` obcina wtedy rysowanie, bo duszek, którego połowa
wypada poza krawędź, to normalny widok.

## Co znaczy zero i kto kogo zasłania

Kiedy duszki trafiają do bufora, obowiązują dwie reguły. Pierwsza dotyczy przezroczystości:
`compose_sprite` wpisuje kolor tylko wtedy, gdy ten kolor jest różny od zera. Kolor numer 0
znaczy „ten piksel nic nie mówi", więc tło pod nim zostaje nietknięte. Dlatego bohater nie
jest prostokątem z niebieskim tłem, tylko ma kształt. W tle zero znaczy coś przeciwnego, bo
w etapie 08 kolor zerowy był wspólnym tłem wszystkich palet. Ten etap znowu ma jedną tabelę
ośmiu kolorów, wspólną dla tła i duszków, jak w etapie 04, ale pod numerem 0 trzyma
niebieskie niebo. Tabela wystarcza, dopóki duszki nie używają zera. Druga reguła siedzi
w tym samym miejscu: co raz trafiło do bufora, to zostaje. Duszek narysowany później zasłania
wcześniejszego, więc kolejność na liście mówi, co jest z przodu:

```c
static sprite_t sprites[SPRITE_COUNT] = {
    { 16,  GROUND_Y - HERO_H,  &hero_bitmaps[0][0][0], HERO_W,  HERO_H  },
    { 128, GROUND_Y - TRUNK_H, &trunk[0][0],           TRUNK_W, TRUNK_H },
    { 200, GROUND_Y - BUG_H,   &bug[0][0],             BUG_W,   BUG_H   },
};
```

Każdy wpis to pozycja w poziomie, pozycja w pionie, wskaźnik na obrazek, a na końcu jego
szerokość i wysokość. `GROUND_Y` to wiersz, w którym zaczyna się trawa, więc bohater
i drzewo stoją na ziemi. `hero_bitmaps[0][0][0]` to pierwszy piksel pierwszej klatki chodu,
a `trunk[0][0]` pierwszy piksel pnia. Kod znajduje bohatera po numerze wpisu, który trzyma
stała `HERO_INDEX`.

Bohater stoi pierwszy, drzewo drugie, więc drzewo zasłania bohatera, kiedy ten za nie
wchodzi. Potem obraz idzie na panel po pasmach, jak w etapie 04: panel nie wie, co w nim
było tłem, a co duszkiem.

## Ruch to jedna liczba na klatkę

Cały ruch to jedna linijka kodu:

```c
hero->x += direction;      /* jedna klatka, jeden piksel */
```

Strzałka w `hero->x` znaczy „pole `x` tego duszka, na który wskazuje `hero`". To skrót od
`(*hero).x`, ta sama gwiazdka z etapu 02. `direction` to kierunek: raz `+1`, raz `-1`.

Nie ma tu zegara ani liczenia czasu: klatka to jedno przejście pętli głównej, więc na
wolniejszym procesorze bohater chodziłby wolniej. Etap 10 odczepi ruch od szybkości
pętli. Druga linijka w `step_hero` podmienia wskaźnik na obrazek: co ósmą klatkę ta sama
postać dostaje nogi w innej pozycji. Obie klatki chodu różnią się trzema wierszami u dołu.
Głowa, ręka z mieczem i pas zostają takie same, a to wystarcza, żeby postać szła, a nie
ślizgała się.

## Dlaczego tło rysujemy od nowa

Rysowanie tła od nowa w każdej klatce wygląda na marnotrawstwo, bo siatka się przecież nie
zmienia. Powód jest ten sam, dla którego w etapie 04 obraz trafił do pamięci: gdyby tło
zostało w buforze z poprzedniej klatki, bohater zostawiłby za sobą czerwony ślad, bo nikt
by go nie zamalował. Można by zamalowywać stary prostokąt bohatera, ale wtedy każdy nowy
rodzaj ruchu wymaga nowego kodu. Cena to 960 kafli na klatkę, czyli 61 440 pikseli: 32
kafle w poziomie razy 30 w pionie, a każdy kafel to osiem na osiem. Kiedy ta cena zacznie
boleć, zmierzymy ją w etapie 15.

## Co powinieneś zobaczyć

Niebo, dwa obłoki, trawę i ścieżkę, kępę trawy, ceglany blok po prawej, wysokie drzewo po
środku, zielonego robaka w trawie i czerwonego rycerza, który idzie w prawo, zawraca na
skraju obrazu i wraca. Kiedy dochodzi do drzewa, chowa się za nie i wychodzi z drugiej
strony. Nogi zmieniają się co osiem klatek, co widać po stopach. Smuga za bohaterem znaczy,
że `compose_background` nie składa tła w każdej klatce; bohater idący przed drzewem znaczy,
że wpisy w `sprites[]` są w złej kolejności.

## Sprawdzenie kompilacji

Z katalogu `tutorial-pl` uruchom `make STAGE=09 check`. Ta komenda tylko kompiluje kod
i nic nie wgrywa, a kompilacja przechodzi bez ostrzeżeń przy `-Wall -Wextra`. Na płytkę
wgrywasz ten sam kod poleceniem z `flash` zamiast `check`; obrazu nie zobaczysz na
komputerze.

## Ćwiczenia

Zamień dwa pierwsze wpisy w tablicy `sprites`, żeby drzewo znalazło się przed bohaterem,
i popraw `HERO_INDEX` na 1, bo po tym numerze kod znajduje bohatera. Rycerz przejdzie wtedy
przed pniem, a nie za nim. O tym, co zasłania co, decyduje sama kolejność wpisów.

Zmień w `main.c` wartość `WALK_MASK` z 7 na 1 i na 63. Kod zmienia nogi wtedy, gdy licznik
klatek ma zera na wszystkich bitach tej maski. Przy 7 wypada to co ósmą klatkę, przy 1 co
drugą (bohater wygląda, jakby biegł w miejscu), a przy 63 prawie nigdy. Wartość w środku
jest kwestią gustu, nie fizyki.

Dodaj czwarty duszek do listy: drugi egzemplarz robaka, postawiony wyżej i przesunięty
w prawo o 40 pikseli. Dopisz jeden wiersz w tablicy i zmień `SPRITE_COUNT` z 3 na 4, bo ta
stała wyznacza rozmiar listy. Ten sam obrazek może wystąpić na liście dwa razy.

Przenieś ruch bohatera z poziomu w pion: w `step_hero` zamień każde `hero->x` na `hero->y`,
a `HERO_LIMIT` na `PANEL_H - HERO_H`. Bohater zacznie chodzić w górę i w dół. Zmień przy tym
krok z jednego piksela na dwa (`HERO_STEP`) i policz, ile klatek zajmuje mu przejście przez
cały ekran.

## Co dalej

Duszki ruszają się w rytmie, w jakim pętla główna zdąży je narysować, a ten rytm zależy od
tego, co jeszcze robi procesor. Konsola rozwiązała to inaczej: kazała układowi obrazu
przerywać pracę procesora w równym rytmie i przesuwała duszki właśnie wtedy. W etapie 10
zobaczymy, jak działa przerwanie, czyli sygnał, który każe procesorowi odłożyć pracę
i wrócić do niej po chwili. Ten sam etap pokaże, po co oddzielić rysowanie od ruchu.
