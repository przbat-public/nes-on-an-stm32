# Etap 02: pamięć i wskaźniki, czyli obraz, który najpierw powstaje w pamięci

Do tej pory każdy piksel leciał na panel w chwili, gdy program go policzył. Dla wzoru
testowego to wystarcza, ale do niczego więcej się nie nadaje. Nie da się przerysować tego,
czego się nie zachowało. Nie da się zmienić jednego fragmentu obrazu, nie rysując całego
od nowa. Nie da się porównać dwóch obrazów. Dlatego przenosimy obraz do pamięci, a ten etap
tłumaczy dwie rzeczy, które to umożliwiają: adres i wskaźnik.

## Adres, czyli numer komórki pamięci

Pamięć mikrokontrolera to długi rząd bajtów, a każdy bajt ma swój numer. Ten numer nazywa
się **adresem**. Kiedy program zapisuje zmienną, kompilator wybiera dla niej miejsce
w tym rzędzie i od tej pory pamięta jego numer.

Adresy wyglądają jak liczby szesnastkowe, na przykład `0x20000000`, i na tym etapie nie
musisz ich znać na pamięć. Wystarczy wiedzieć, że istnieją i że można ich używać.

## Wskaźnik, czyli zmienna, która trzyma adres

**Wskaźnik** to zmienna, która zamiast wartości trzyma adres. Zapisujemy go z gwiazdką:

```c
uint8_t *pixels;
```

`uint8_t` to typ liczby mieszczącej się w jednym bajcie, czyli od 0 do 255; taką liczbą
opisujemy jeden piksel. Gwiazdka przy typie znaczy „adres miejsca, w którym leży bajt".
Dwa znaki trzeba rozróżniać, bo znaczą co innego:

```c
pixels++;      /* przesuń wskaźnik na następny bajt */
*pixels = 5;   /* zapisz piątkę pod adres, który wskaźnik trzyma */
```

Znak `&` przed zmienną daje jej adres:

```c
uint8_t value = 5;
uint8_t *where = &value;   /* where wskazuje na value */
```

Ten sam znak `&` ma w C drugie znaczenie: między dwiema liczbami zostawia tylko te bity,
które są włączone w obu. Zobaczysz taki zapis w `push_framebuffer`, gdzie z bajtu obrazu
wybiera numer koloru.

To wszystko, co trzeba wiedzieć, żeby czytać kod tego etapu.

## Tablica to adres

W etapie 01 tablica była zbiorem wartości pod jednym nazwiskiem. Teraz można powiedzieć
więcej: **nazwa tablicy to adres jej pierwszego elementu**. Dlatego te dwa zapisy robią
dokładnie to samo:

```c
pixels[i] = 5;     /* sięgnij i-ty element */
*(pixels + i) = 5; /* policz adres o i bajtów dalej i zapisz pod nim */
```

W kodzie tego etapu są dwie funkcje: `fill_with_array` sięga po bajty nawiasami
kwadratowymi, a `fill_with_pointer` przesuwa wskaźnik. Obie robią to samo. Zobaczysz w nich,
że to jeden mechanizm, a nie dwie sztuczki.

## Bufor obrazu, czyli obraz jako rząd bajtów

Obraz w tym etapie to jedna duża tablica bajtów: po jednym bajcie na piksel, 320 na 240,
czyli 76 800 bajtów. Kilobajt to 1024 bajty, więc bufor zajmuje 75 z 96 kilobajtów pamięci,
którą ma do dyspozycji program. To ponad trzy czwarte. Na zmienne i resztę programu zostaje
21 kilobajtów, a drugi taki obraz już by się nie zmieścił.

Pierwszy bajt należy do piksela w lewym górnym rogu, drugi do piksela obok, aż do końca
wiersza, po którym zaczyna się następny. Żeby trafić do piksela w kolumnie `x` i wierszu
`y`, trzeba więc policzyć `y * PANEL_W + x`. Ta arytmetyka to cała dwuwymiarowa grafika
na tym etapie.

Dlaczego jeden bajt na piksel, a nie kolor? Bo kolor zajmuje dwa bajty, więc obraz
w kolorach miałby 153 600 bajtów, czyli 150 kilobajtów. Mikrokontroler ma 96 kilobajtów
pamięci, więc taki obraz by się nie zmieścił. Bufor trzyma **numer koloru**, a nie kolor,
i dlatego mieści się w pamięci. Numer zamieniamy na kolor dopiero wtedy, gdy obraz wyrusza
na panel, w jednej krótkiej pętli. Numer to pozycja w tablicy `palette`; ile ta tablica ma
miejsc, mówi stała `COLOUR_COUNT` (w etapie 01 liczyła to `PALETTE_SIZE`).

Słowo `static` przed tablicą znaczy to samo, co przy zmiennej `bands` z etapu 01. Taka
zmienna istnieje przez cały czas działania programu: dostaje miejsce w pamięci jeszcze przed
pierwszym rozkazem `main`, a nie dopiero wtedy, gdy ktoś jej użyje.

## Dlaczego to jest lepsze

Program rysuje teraz scenę w pamięci: niebieskie niebo, zieloną trawę, biały prostokąt,
czerwoną wieżę i ukośną kreskę. Funkcja `set_pixel` zmienia jeden bajt i nic więcej. Dopiero
na końcu funkcja `push_framebuffer` wysyła cały obraz na panel.

Obraz można zmieniać dowolnie, zanim ktokolwiek go zobaczy: rysować warstwami, poprawiać,
porównywać z poprzednim. Właśnie tej możliwości potrzebuje emulator konsoli, żeby w ogóle
powstać: obraz konsoli powstaje w pamięci, a panel dostaje gotowy.

## Co powinieneś zobaczyć

Niebieskie niebo, zieloną trawę na dole, biały prostokąt, czerwoną wieżę z żółtym pasem
i ukośną białą kreskę po prawej.

Obraz powstaje w dwóch krokach. Najpierw `draw_scene()` zmienia bajty w pamięci i na panelu
nie dzieje się nic. Dopiero `push_framebuffer()` pokazuje wynik. Sprawdzisz to, usuwając
z `main` wywołanie `push_framebuffer()`: obraz powstanie w pamięci, ale na panel nie trafi
ani jeden piksel.

## Ćwiczenia

Po każdej zmianie wgraj program tak samo jak w etapie 00: `make STAGE=02 flash` z katalogu
`tutorial-pl`.

Przenieś biały prostokąt w inne miejsce, zmieniając dwie liczby w wywołaniu
`draw_rectangle`. Zauważ, że nie musisz przy tym nic przeliczać ręcznie.

Narysuj ramkę wokół całego obrazu, wywołując cztery prostokąty: górny, dolny, lewy i prawy.
To ćwiczenie pokazuje, po co są funkcje: cztery wywołania zamiast czterech pętli.

`set_pixel` przy próbie wyjścia za obraz nie robi nic i na razie tego nie zobaczysz, bo
wszystko mieści się w środku. W `draw_scene` przesuń ukośną kreskę tak, żeby wychodziła
za prawą krawędź, na przykład na `set_pixel(300 + i, 40 + i, 7)`. Potem zmień `set_pixel`
tak, żeby zamiast pomijać piksele spoza obrazu zawijała je na przeciwną stronę. Zobacz,
co się wtedy stanie z kreską.

Napisz funkcję, która kopiuje jeden wiersz obrazu, czyli 320 bajtów, do osobnej tablicy,
używając wyłącznie wskaźników. Całego bufora skopiować nie możesz, bo drugi taki obraz
nie mieści się w pamięci.

## Co dalej

W etapie 03 zajmiemy się czasem: dowiesz się, po co mikrokontrolerowi zegar i jak zrobić,
żeby obraz zmieniał się sam, bez wgrywania programu od nowa. Dotąd program rysował jeden
obraz i nic więcej się nie działo; teraz zacznie rysować je bez końca.
