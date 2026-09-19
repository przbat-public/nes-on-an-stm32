# Etap 02: pamięć i wskaźniki, czyli obraz, który najpierw powstaje w pamięci

Do tej pory każdy piksel leciał na panel w chwili, gdy program go policzył. Dla wzoru
testowego to wystarcza, ale do niczego więcej się nie nadaje. Nie da się przerysować tego,
czego się nie zachowało. Nie da się zmienić jednego fragmentu obrazu, nie rysując całego
od nowa. Nie da się porównać dwóch obrazów. Dlatego obraz przenosi się do pamięci, a ten
etap tłumaczy dwie rzeczy, które to umożliwiają: adres i wskaźnik.

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

Gwiazdka przy typie znaczy „adres miejsca, w którym leży bajt". Dwa znaki trzeba rozróżniać,
bo znaczą co innego:

```c
pixels++;      /* przesuń wskaźnik na następny bajt */
*pixels = 5;   /* zapisz piątkę pod adres, który wskaźnik trzyma */
```

Znak `&` przed zmienną daje jej adres:

```c
uint8_t value = 5;
uint8_t *where = &value;   /* where wskazuje na value */
```

To wszystko, co trzeba wiedzieć, żeby czytać kod tego etapu.

## Tablica to adres

W etapie 01 tablica była zbiorem wartości pod jednym nazwiskiem. Teraz można powiedzieć
więcej: **nazwa tablicy to adres jej pierwszego elementu**. Dlatego te dwa zapisy robią
dokładnie to samo:

```c
pixels[i] = 5;     /* sięgnij i-ty element */
*(pixels + i) = 5; /* przesuń wskaźnik o i bajtów i zapisz pod nim */
```

W kodzie etapu są dwie funkcje, `fill_with_array` i `fill_with_pointer`, które robią to
samo, jedna przez nawiasy kwadratowe, druga przez przesuwanie wskaźnika. Napisałem obie,
żebyś zobaczył, że to jest ten sam mechanizm, a nie dwie różne sztuczki.

## Bufor obrazu, czyli obraz jako rząd bajtów

Obraz w tym etapie to jedna duża tablica bajtów: po jednym bajcie na piksel, 320 na 240,
czyli 76 800 bajtów. Pierwszy bajt należy do piksela w lewym górnym rogu, drugi do piksela
obok, aż do końca wiersza, po którym zaczyna się następny. Żeby trafić do piksela w kolumnie
`x` i wierszu `y`, trzeba więc policzyć `y * PANEL_W + x`. Ta arytmetyka to cała dwuwymiarowa
grafika na tym etapie.

Dlaczego jeden bajt na piksel, a nie kolor? Bo kolor zajmuje dwa bajty, więc obraz
w kolorach miałby 153 600 bajtów. Mikrokontroler ma 96 kilobajtów pamięci, więc taki obraz
by się nie zmieścił. Bufor trzyma więc **numer koloru**, a nie kolor, i dlatego mieści się
w pamięci. Numer zamieniamy na kolor dopiero wtedy, gdy obraz wyrusza na panel, w jednej
krótkiej pętli.

## Dlaczego to jest lepsze

Program rysuje teraz scenę w pamięci: niebo, trawę, biały prostokąt, czerwona wieża i przekątna
kreska. Funkcja `set_pixel` zmienia jeden bajt i nic więcej. Dopiero na końcu jedna funkcja
`push_framebuffer` wysyła cały obraz na panel.

Zysk jest taki, że obraz można zmieniać dowolnie, zanim ktokolwiek go zobaczy: rysować
warstwami, poprawiać, porównywać z poprzednim. To jest dokładnie ta możliwość, której
emulator potrzebuje, żeby w ogóle powstać: obraz konsoli powstaje w pamięci klatka po klatce,
a panel dostaje gotowy.

## Co powinieneś zobaczyć

Niebieskie niebo, zieloną trawę na dole, biały prostokąt, czerwoną wieżę z żółtym pasem
i ukośną białą kreskę po prawej.

Ważne jest to, że obraz powstaje w dwóch krokach. Najpierw `draw_scene()` zmienia bajty
w pamięci i na panelu nie dzieje się nic. Dopiero `push_framebuffer()` pokazuje wynik.
Możesz to sprawdzić, zakomentowując drugie wywołanie.

## Ćwiczenia

Przenieś biały prostokąt w inne miejsce, zmieniając dwie liczby w wywołaniu
`draw_rectangle`. Zauważ, że nie musisz przy tym nic przeliczać ręcznie.

Narysuj ramkę wokół całego obrazu, wywołując cztery prostokąty: górny, dolny, lewy i prawy.
To ćwiczenie pokazuje, po co są funkcje: cztery wywołania zamiast czterech pętli.

Zmień `set_pixel` tak, żeby przy próbie wyjścia za obraz nie robiła nic (już tak robi),
a potem spraw, żeby zawijała współrzędne na przeciwną stronę. Zobacz, co się stanie
z przekątną.

Napisz funkcję, która kopiuje cały bufor do drugiego bufora, używając wyłącznie wskaźników.
To dokładnie ta operacja, której emulator potrzebuje, żeby porównać dwie klatki.

## Co dalej

W etapie 03 zajmiemy się czasem: dowiesz się, po co mikrokontrolerowi zegar i jak to zrobić,
żeby obraz zmieniał się sam, bez wgrywania programu od nowa. To pierwszy etap, w którym
program działa w pętli i coś się w nim dzieje.
