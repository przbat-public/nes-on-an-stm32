# Etap 01: język programu, czyli zmienne, funkcje, pętle i warunki

W etapie 00 napisałeś program, który jest jedną długą listą poleceń. Działa, ale przy
bardziej złożonym programie utonąłbyś w powtarzaniu tych samych linii. Ten etap nie zmienia
niczego w obsłudze panelu, a zajmuje się wyłącznie językiem, w którym te polecenia
zapisujemy. Większość nowych pojęć zmienia przy tym obraz na panelu, więc od razu widać,
co dane pojęcie robi.

## Stałe, czyli liczby, które mają nazwy

W etapie 00 kolor nosił już swoją nazwę: `COLOUR_BLUE`. Ten etap pokazuje, dlaczego tak się
to robi. Liczba wpisana w kod bezpośrednio nazywa się **literałem** i ma jedną wadę: nic nie
mówi. Kiedy po tygodniu wrócisz do pliku, nie będziesz pamiętał, czym jest `0x001F`.

Stała to liczba z nazwą. Zapisujemy ją słowem `#define`:

```c
#define COLOUR_BLUE   0x001F
#define PANEL_W       320
#define PANEL_H       240
```

Od tej chwili w kodzie piszesz `COLOUR_BLUE`, a nie liczbę. Kompilator podmienia nazwę
na liczbę jeszcze przed właściwym tłumaczeniem, więc stała nie kosztuje nic w czasie
działania programu. Zysk jest podwójny: kod czyta się jak opis, a wartość mieszka
w jednym miejscu.

## Zmienne, czyli wartości, które mogą się zmieniać

Stała jest z definicji niezmienna. **Zmienna** to miejsce w pamięci, które ma nazwę
i wartość. Tę wartość można zmieniać w trakcie działania programu:

```c
static int bands = 8;
```

`int` to typ, czyli rodzaj wartości: liczba całkowita. `bands` to nazwa, `= 8` to wartość
początkowa. Słowo `static` przyjmij na razie jako „ta zmienna istnieje przez cały czas
działania programu"; wrócimy do niego w etapie 02, gdy zajmiemy się pamięcią.

Zmienna `bands` mówi, na ile pasów dzielimy ekran. Zmień jej wartość na 4, potem na 16,
a za każdym razem zobaczysz inny obraz. Na tym polega różnica między stałą a zmienną:
jedną ustawiasz raz, drugą zmieniasz, żeby zobaczyć, co się stanie.

## Tablica, czyli wiele wartości pod jednym nazwiskiem

Kolory, których używamy, to osiem liczb tego samego typu. Trzymanie ich w ośmiu osobnych
zmiennych byłoby niewygodne, bo w pętli nie da się powiedzieć „weź następny kolor".
**Tablica** pozwala trzymać je razem i sięgać po nie numerem:

```c
static const uint16_t palette[] = {
    COLOUR_BLACK, COLOUR_RED, COLOUR_GREEN, COLOUR_BLUE,
    COLOUR_YELLOW, COLOUR_MAGENTA, COLOUR_CYAN, COLOUR_WHITE,
};
```

`uint16_t` to typ liczby mieszczącej się w dwóch bajtach; tyle zajmuje jeden kolor. `const`
znaczy „nie wolno zmieniać": kolory ustalamy raz i tablica ma tak zostać.

Numer, którym sięgamy po element, nazywa się **indeksem** i liczy się od zera. Dlatego
`palette[0]` to czarny, a `palette[7]` to biały. Najczęstsze potknięcie początkujących:
pierwszy element ma indeks zero, nie jeden.

Ile elementów ma tablica, mówi `PALETTE_SIZE`. Ta stała dzieli rozmiar całej tablicy przez
rozmiar jednego elementu, więc liczba wychodzi sama, nawet gdy dopiszesz kolejny kolor.

## Funkcja, czyli kawałek pracy z nazwą

W etapie 00 ekran wypełniała funkcja `fill`, która wysyłała na panel piksel po pikselu.
Teraz wydzielamy z niej funkcję, która umie wypełnić dowolny prostokąt:

```c
static void fill_rectangle(int x0, int y0, int w, int h, uint16_t colour)
```

Nazwa `fill_rectangle` mówi, co funkcja robi. W nawiasach stoją **parametry**, czyli
wartości, które funkcja dostaje na wejściu, każda z typem. W miejscu typu zwracanego stoi
`void`, co znaczy „ta funkcja nic nie zwraca". Gdyby stało tam `int`, funkcja musiałaby
kończyć się słowem `return` z liczbą.

Dzięki wydzieleniu pętla po pikselach jest w całym pliku napisana raz. Znajdziesz w niej
błąd i poprawisz go w jednym miejscu, a nie w każdym, w którym wypełniasz ekran.

Zanim pójdziesz dalej, spójrz, w jakiej kolejności czytać `main.c`. Pierwsza linia,
`#include <stdint.h>`, dołącza do programu gotowe definicje typów, między innymi `uint16_t`.
Dalej plik układa się z góry na dół: najpierw stałe i tablica kolorów, bo muszą być znane,
zanim ktokolwiek po nie sięgnie, potem funkcje, a na samym końcu `main`. Wykonanie programu
zaczyna się właśnie od `main`, choć leży najniżej w pliku: to `main` wywołuje funkcje,
a nie odwrotnie.

## Pętla, czyli powtarzanie bez kopiowania

Pętla `for` powtarza to samo polecenie wiele razy:

```c
for (int i = 0; i < bands; i++) {
    ...
}
```

W nawiasie są trzy części rozdzielone średnikami: ustawienie licznika (`int i = 0`), warunek
trwania (`i < bands`) i to, co dzieje się po każdym obrocie (`i++`, czyli zwiększ o jeden).
Pętla wykonuje się tak długo, jak warunek jest prawdziwy.

Pętla w `draw_bands` liczy pasy, a w `fill_rectangle` dwie pętle idą po wierszach i po
pikselach. Pętla w pętli to normalny sposób rysowania obrazu.

Po który kolor sięgnąć w danym pasie, mówi reszta z dzielenia: `i % PALETTE_SIZE`. Gdy pasów
jest więcej niż kolorów, tablica po prostu zaczyna się od nowa.

## Warunek, czyli decyzja

**Warunek** pozwala wykonać coś tylko wtedy, gdy coś jest prawdą:

```c
if (i == bands - 1) {
    colour = COLOUR_WHITE;
}
```

Dwa znaki równości to porównanie, jeden znak to przypisanie. Pomylenie ich to kolejne
klasyczne potknięcie: `if (i = 5)` przypisze pięć do `i` i zawsze będzie prawdziwe,
a `if (i == 5)` zapyta, czy `i` wynosi pięć.

W naszym programie warunek sprawia, że ostatni pas jest biały niezależnie od tego, co mówi
tablica kolorów, więc zawsze widać, gdzie kończy się obraz. Przy domyślnych ośmiu pasach
nie zobaczysz jednak różnicy, bo ósmy kolor w tablicy to i tak biel. Zmień `bands` na 7
albo na 4 i wtedy warunek naprawdę coś zmieni.

## Co powinieneś zobaczyć

Osiem poziomych pasów w kolorach z tablicy. Ostatni jest biały, więc widać, gdzie kończy
się obraz. Cały obraz powstaje z jednego wywołania `draw_bands()` w `main`, a nie z listy
poleceń.

## Ćwiczenia

Po każdej zmianie wgraj program tak samo jak w etapie 00: `make STAGE=01 flash` z katalogu
`tutorial-pl`.

Zmień `bands` na 4, potem na 16, a potem na 7. Zauważ, co się dzieje, gdy liczba pasów nie
dzieli równo wysokości ekranu: dzielenie całkowite obcina resztę, więc na dole zostaje
niewykorzystany pasek. Zastanów się, jak to poprawić.

Dodaj warunek, który maluje co trzeci pas na czarno. Podpowiedź: reszta z dzielenia,
czyli `i % 3`, wynosi zero dla co trzeciego elementu.

Dodaj do tablicy dwa własne kolory i użyj ich. Tablica ma tyle elementów, ile wpisów,
a `PALETTE_SIZE` liczy je za ciebie.

Napisz funkcję `fill_square(int x, int y, int size, uint16_t colour)`, która maluje kwadrat,
wywołując `fill_rectangle`. To pokazuje, po co są funkcje: jedna zbudowana na drugiej,
a żadna nie powtarza pętli po pikselach.

## Co dalej

W etapie 02 zajmiemy się pamięcią: dowiesz się, czym jest wskaźnik i dlaczego obraz
wygodniej trzymać w tablicy w pamięci, a na panel wysyłać dopiero gotowy. Adres, który
w etapie 00 był tylko numerem podawanym przy wgrywaniu programu, stanie się tam narzędziem:
będziesz go liczyć i przesuwać.
