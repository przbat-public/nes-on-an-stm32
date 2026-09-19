# Etap 10: czas w konsoli, czyli przerwania i klatki

Etapy 03 i 09 ruszały obraz, ale tempo ruchu zależało od tego, jak szybko pętla zdążyła
narysować i wysłać klatkę: na wolniejszym procesorze wszystko szłoby wolniej, a każda zmiana
w kodzie zmieniałaby szybkość. Etap 03 odmierzał czas funkcją `delay_ms`, czyli liczeniem
obrotów pętli. Mówił przy tym wprost, że dokładnie mierzy czas tylko układ, który sam liczy
takty zegara. Ten etap takiego układu używa i pokazuje, po co konsola przerywała procesorowi
pracę sześćdziesiąt razy na sekundę.

## Przerwanie, czyli sygnał, którego nie da się przegapić

Dotąd program sam pytał sprzęt, czy coś się stało. Funkcja `spi_byte` stoi w pętli i sprawdza
`SPI1_SR`, aż układ będzie gotowy przyjąć kolejny bajt. To **odpytywanie**: program zadaje
pytanie milion razy, a odpowiedź prawie zawsze brzmi „jeszcze nie".

**Przerwanie** odwraca tę zależność. Program nie pyta o nic, a układ sam sygnalizuje, że
nadszedł jego moment. Procesor odkłada wtedy to, co robił, skacze pod adres zapisany dla tego
sygnału, wykonuje funkcję i wraca dokładnie w to samo miejsce.

## Gdzie rdzeń szuka obsługi

Adresy wszystkich obsług leżą w tabeli przerwań, na samym początku pamięci programu: jedno
miejsce na każdy sygnał. Kiedy sygnał przychodzi, rdzeń czyta z tabeli właściwy adres
i tam skacze.

Nasza tabela pochodzi z pliku startowego pożyczonego z emulatora i w każdym miejscu ma
domyślną obsługę, która wiruje w miejscu. Nie ma tam nazwy, którą dałoby się podmienić,
a pamięci programu nie da się zapisywać w trakcie działania. Dlatego funkcja `vectors_init`
zapisuje tę tabelę jeszcze raz w pamięci danych i w miejscu zegara kładzie adres
`SysTick_Handler`. Na końcu mówi rdzeniowi, gdzie ma tej tabeli szukać: to rejestr `SCB_VTOR`,
czyli wskaźnik na tabelę przerwań. Od tego momentu sygnał z zegara trafia do naszej funkcji;
bez tego pierwszy sygnał zatrzymałby program na zawsze.

## Zegar, który liczy sam

Skąd sygnał, skoro nie ma przycisku ani niczego z zewnątrz? W samym rdzeniu siedzi licznik,
który odejmuje jeden na każdy takt procesora. Wpisujesz do niego liczbę, on schodzi do zera,
zgłasza przerwanie i zaczyna od nowa. Nazywa się SysTick, a w kodzie występuje pod trzema
nazwami zaczynającymi się od `SYST_`: jedna trzyma okres, druga stan licznika, trzecia go
włącza. Okres ustawia się jednym dzieleniem:

```
80 000 000 taktów na sekundę / 60 = 1 333 333 takty na jedno przerwanie
```

Wpisujemy o jeden mniej, bo pełny okres to wpisana liczba plus jeden takt: licznik schodzi
do zera, a dopiero w następnym takcie ładuje się od nowa. W kodzie stoją za tym trzy stałe:
`CORE_HZ`, `FPS` (od angielskiego *frames per second*, klatki na sekundę) i `TICK_CYCLES`,
czyli wynik dzielenia. To jedyne miejsce, w którym tempo programu zależy od zegara z etapu 04.

Czego w nim nie ma: liczby zmiennoprzecinkowej. 1 333 333 to 1 333 333,33 obcięte do całości,
więc okres wypada o cztery nanosekundy krótszy od jednej sześćdziesiątej sekundy. Po minucie
zbiera się z tego tysięczna część klatki. Konsola żyła z gorszymi błędami i my też możemy.

## Obsługa przerwania robi dwie rzeczy

Licznik `ticks` i flaga `frame_ready`, nic więcej. Flaga to zmienna, która mówi „stało się".
Kusi, żeby przenieść do obsługi rysowanie, bo „i tak zaraz trzeba narysować klatkę". Nie wolno,
i to nie z ostrożności, tylko z arytmetyki: przerwanie zatrzymuje wszystko, co właśnie się
dzieje, więc im dłużej trwa, tym mniej czasu zostaje na resztę programu. Rysowanie całego obrazu
to kilka milisekund, a na jedno przerwanie przypada szesnaście. Obraz powstały w obsłudze
pociąłby się jeszcze inaczej: pasma wysłane przed przerwaniem pochodziłyby z jednego rysunku,
a te po niej z następnego.

Na licznik trzeba umieć poczekać i to jest cała rola funkcji `wait_for_tick`. Najpierw zeruje
flagę, a potem stoi w pustej pętli, dopóki obsługa nie podniesie jej z powrotem. Kolejność ma
znaczenie: gdyby zerowała flagę po czekaniu, sygnał, który przyszedł między odczyt flagi a jej
zerowanie, przepadłby i program czekałby o jedno przerwanie dłużej. Na czas zerowania przerwania
są wyłączone, żeby sygnał nie wpadł w środek tej operacji.

Obie zmienne mają przy typie słowo `volatile`, które mówi kompilatorowi: nie zakładaj, że tylko
ty je zmieniasz. Bez tego słowa kompilator wyrzuciłby całe sprawdzanie flagi.

## Dwa rytmy, które trzeba rozdzielić

Konsola tworzyła obraz sześćdziesiąt razy na sekundę i nic nie wiedziała o tym, gdzie ten obraz
trafia. Panel łączy się z płytką jednym łączem SPI: bajty lecą po nim jeden za drugim. Przy
40 MHz z etapu 04 drut przenosi 40 milionów bitów na sekundę, czyli około 5 milionów bajtów,
bo bajt to osiem bitów. Jeden obraz 256 na 240 pikseli to 122 880 bajtów. W jednej
sześćdziesiątej sekundy, czyli w 16,67 milisekundy, drut zmieści jakieś 83 tysiące bajtów.
Półtora raza za mało.

- jedno pasmo ośmiu linii to 4096 bajtów, czyli 0,82 milisekundy na drucie. W okresie
  16,67 milisekundy zostaje więc jakieś 95% czasu dla programu,
- jedno okrążenie obrazu to 30 pasm, czyli 30 przerwań, plus jedno na sam początek: pół sekundy.

Obraz odświeża się więc dwa razy na sekundę, a pozycję wzoru wyznacza licznik przerwań, który
rośnie sześćdziesiąt razy na sekundę. Te dwa rytmy muszą w programie działać osobno: gdyby
rysowanie i wysyłanie szły jednym ciągiem bez czekania, pasma leciałyby tak szybko, jak pozwala
drut, a szybkość ruchu zależałaby od długości wykonywanego kodu.

## Pętla główna

W pętli głównej są trzy kroki, każdy odpowiada jednej rzeczy z tego rozdziału:

1. `wait_for_tick()` — czekaj na sygnał i weź numer okrążenia,
2. `draw_pattern(frame)` — narysuj cały obraz w pamięci, korzystając z tego numeru,
3. pętla po pasmach — wyślij pasmo i poczekaj na kolejny sygnał.

Krok drugi korzysta z licznika, bo to on jest jedynym zegarem w programie. Wzór jest umyślnie
prosty, bo w tym etapie chodzi o tempo, a nie o obraz; scena z duszkiem wraca dopiero
w jedenastym. Fragment wzoru pyta, w którym pasmie ośmiu pikseli leży dany punkt, licząc `x`
i `y` razem, i dodaje numer okrążenia:

```c
int stripe = ((x + y + (int)frame) >> 3) & 3;
```

`>> 3` to przesunięcie bitów w prawo, czyli dzielenie przez osiem. `& 3` to maska: zostawia dwa
najmłodsze bity, czyli numer jednego z czterech pasów. Z tych dwóch liczb biorą się obie potęgi
dwójki: szerokość pasa to 2³, a liczba pasów to 2². Inaczej się nie da, bo przesunięcie i maska
pracują na pojedynczych bitach. Pas zerowy idzie na biało (`(stripe == 0) ? 7 : stripe`), więc
na ekranie widać cztery kolory: biały, czerwony, zielony i niebieski.

Obraz powstaje raz na 31 przerwień, a wzór powtarza się co 32 piksele (cztery pasy po osiem).
Te dwie liczby prawie się znoszą: z 31 przerwień zostaje na ekranie przesunięcie o jeden piksel.
Te same piksele dostają inne kolory, a oko czyta to jako ruch. Ruch zależy tylko od licznika
okrążeń: gdyby rysowanie było dwa razy szybsze, obraz ruszałby się tak samo.

## Co powinieneś zobaczyć

Ukośne pasy w czterech kolorach: białym, czerwonym, zielonym i niebieskim. Suną wolno po
ekranie, o jeden piksel na każde odświeżenie, a w lewym górnym narożniku biały kwadracik stoi
w miejscu. Kwadracik stoi celowo: gdyby cały obraz był ruchomy, nie dałoby się odróżnić ruchu
od migotania.

Obraz nie migocze i to jest druga rzecz do obejrzenia. Każde pasmo powstaje z obrazu już
narysowanego w pamięci, więc panel nigdy nie pokazuje mieszanki dwóch klatek. Gdyby rysowanie
trafiło do obsługi przerwania, pasy połamałyby się na granicach pasm.

## Zbuduj i sprawdź

Z katalogu `tutorial-pl`:

```bash
make STAGE=10 check    # sam build, bez ostrzeżeń przy -Wall -Wextra
make STAGE=10 flash    # zbuduj i wgraj na płytkę
```

Pierwsza komenda to cała weryfikacja z tego przewodnika: kompilacja bez ani jednego
ostrzeżenia.

## Ćwiczenia

Zmień w `draw_pattern` przesunięcie `>> 3` na `>> 4`, a potem na `>> 2`. Pasy robią się szersze
albo węższe, a `& 3` dalej zostawia cztery kolory. Zastanów się, dlaczego i szerokość pasa,
i liczba kolorów muszą być potęgami dwójki.

Usuń z `wait_for_tick` zerowanie flagi. Po pierwszym przerwaniu flaga zostaje jedynką, więc
każde czekanie kończy się od razu: pasma lecą tak szybko, jak pozwala drut, a obraz zmienia się
kilkadziesiąt razy na sekundę i sunie dużo szybciej niż przed zmianą. Ruch nadal liczy się
z zegara, ale rytm znika.

Przenieś `draw_pattern` do obsługi przerwania i zobacz, co się dzieje z obrazem. Każde pasmo
pochodzi wtedy z rysunku wykonanego o jedno przerwanie później, więc sąsiednie pasma różnią się
o piksel, a ukośne krawędzie robią się schodkowe.

Zmień `FPS` z 60 na 30, a potem na 120. Timer dzieli zegar na dłuższy albo krótszy okres, więc
obraz zwalnia albo przyspiesza równo dwa razy. Przy 120 drut pracuje dwa razy częściej: pasm
na sekundę jest 120, a każde zajmuje te same 0,82 milisekundy.

Zmień w `draw_pattern` krok ruchu z `(int)frame` na `2 * (int)frame`. Pasy suną dwa razy
szybciej, choć przerwania przychodzą tak samo często. Ruch zależy od kodu, tempo od zegara.

## Co dalej

W etapie 11 konsola zaczyna czytać przyciski, więc obraz przestaje zmieniać się sam. Wracają
też duszki z etapu 09, tym razem posłuszne drążkowi.
