# Etap 01: język programu, czyli zmienne, funkcje, pętle i warunki

W etapie 00 napisałeś program, który jest jedną długą listą poleceń. Działa, ale gdyby miał
robić coś bardziej złożonego, utopiłbyś się w powtarzaniu tych samych linii. Ten etap
zostawia panel i kolory bez zmian, a zajmuje się wyłącznie językiem, w którym te polecenia
zapisujemy. Po każdym nowym pojęciu obraz na panelu zmienia się w widoczny sposób, więc
od razu widać, co dane pojęcie robi.

## Stałe, czyli liczby, które mają nazwy

W poprzednim programie kolor był liczbą wstawioną w miejsce, w którym akurat był potrzebny.
Taka liczba w kodzie nazywa się **literałem** i ma jedną wadę: nic nie mówi. Kiedy po
tygodniu wrócisz do pliku, nie będziesz pamiętał, czym jest `0x001F`.

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
i wartość, i którą można zmieniać w trakcie programu:

```c
static int bands = 8;
```

`int` to typ, czyli rodzaj wartości: liczba całkowita. `bands` to nazwa, `= 8` to wartość
początkowa. Słowo `static` przyjmij na razie jako „ta zmienna istnieje przez cały czas
działania programu"; wrócimy do niego, gdy będziemy mówić o pamięci.

Zmienna `bands` mówi, na ile pasów dzielimy ekran. Zmień jej wartość na 4, potem na 16,
a za każdym razem zobaczysz inny obraz. Na tym polega różnica między stałą a zmienną:
jedną ustawiasz raz, drugą zmieniasz, żeby zobaczyć, co się stanie.

## Tablica, czyli wiele wartości pod jednym nazwiskiem

Kolory, których używamy, to osiem liczb tej samej wielkości i tego samego rodzaju. Trzymanie
ich w ośmiu osobnych zmiennych byłoby niewygodne, bo w pętli nie da się powiedzieć „weź
następny kolor". **Tablica** pozwala trzymać je razem i sięgać po nie numerem:

```c
static const uint16_t palette[] = {
    COLOUR_BLACK, COLOUR_RED, COLOUR_GREEN, COLOUR_BLUE,
    COLOUR_YELLOW, COLOUR_MAGENTA, COLOUR_CYAN, COLOUR_WHITE,
};
```

Numer, którym sięgamy po element, nazywa się **indeksem** i liczy się od zera. Dlatego
`palette[0]` to czarny, a `palette[7]` to biały. Pierwszy element pod indeksem zerowym
to najczęstsze potknięcie początkujących i warto je zapamiętać od razu.

## Funkcja, czyli kawałek pracy z nazwą

W etapie 00 wypełnianie ekranu było pętlą w `main`. Teraz wydzielamy je do funkcji:

```c
static void fill_rectangle(int x0, int y0, int w, int h, uint16_t colour)
```

Czytamy to od środka. Nazwa `fill_rectangle` mówi, co funkcja robi. W nawiasach stoją
**parametry**, czyli wartości, które funkcja dostaje na wejściu, każda z typem. Słowo
`void` przed nazwą to **typ zwracany**: `void` znaczy „ta funkcja nic nie zwraca".
Gdyby stało tam `int`, funkcja musiałaby kończyć się słowem `return` z liczbą.

Dzięki wydzieleniu pętla po pikselach jest w całym pliku napisana raz. Znajdziesz w niej
błąd, poprawiasz go w jednym miejscu, a nie w każdym miejscu, w którym wypełniasz ekran.

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

W funkcji `draw_bands` pętla liczy pasy, a druga pętla, wewnątrz `fill_rectangle`, liczy
piksele. Jedna pętla wewnątrz drugiej to normalny sposób rysowania obrazu.

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
tablica kolorów, więc zawsze widać, gdzie kończy się obraz.

## Co powinieneś zobaczyć

Osiem poziomych pasów w kolorach z tablicy, a ostatni biały. Cały obraz powstaje z jednego
wywołania `draw_bands()` w `main`, a nie z listy poleceń.

## Ćwiczenia

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
wygodniej trzymać w tablicy w pamięci, a na panel wysyłać dopiero gotowy. To pierwszy
etap, w którym pojawi się coś, czego w etapie 00 nie było: adres.
