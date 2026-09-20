# Emulator NES na STM32: przewodnik od podstaw

Ten katalog to przewodnik pisany po polsku. Prowadzi czytelnika od pustego okna edytora
do działającego emulatora konsoli, którą wielu z nas znało jako Pegasusa. Kod i komentarze
w repozytorium są po angielsku, tekst nauki jest polski.

## Dla kogo

Dla kogoś, kto nie napisał jeszcze żadnego programu i nigdy nie wgrywał niczego na płytkę.
Nie zakładamy znajomości języka C. Nie zakładamy, że wiesz, czym jest kompilator, jak działa
pamięć programu ani co się dzieje, kiedy naciskasz przycisk reset. Wszystko, czego
potrzebujesz, wprowadzamy wtedy, gdy staje się potrzebne: najpierw narzędzia i pierwszy
uruchomiony program, potem zmienne i funkcje, potem wskaźniki, a dopiero potem konsola.

Nie znaczy to, że przewodnik jest łatwy. Kończy się na emulacji procesora, układu obrazu
i kartridży z przełączaniem pamięci, czyli na rzeczach, które zwykle poznaje się po latach.
Trudność rośnie po jednym stopniu, a każdy stopień kończy się czymś, co widzisz na panelu
albo w wyniku testu.

Ten katalog leży w repozytorium, w którym działa już gotowy emulator. Przewodnik nie streszcza
tego kodu. Prowadzi do niego własną, prostszą drogą, a gotowy emulator służy jako punkt
odniesienia w końcowych rozdziałach.

## Jak uczymy

Trzy zasady, każda z konkretnego powodu:

1. **Najpierw robimy, potem tłumaczymy.** Każdy etap ma jeden wynik: program, który działa,
   i obraz, który można sprawdzić okiem. Podział na typy dokumentacji pochodzi z metodyki
   [Diátaxis](https://diataxis.fr/pl/start-here/): ten katalog jest przewodnikiem,
   a `docs/ARCHITECTURE.md` i `docs/PERFORMANCE.md` w repozytorium pełnią rolę wyjaśnienia
   i referencji.
2. **Kompletny przykład zamiast opisu.** Etap to gotowy, uruchamialny program, a wyjaśnienie
   stoi obok kodu, nie w osobnym rozdziale. Tak działa
   [efekt przykładu rozpracowanego](https://dl.acm.org/doi/pdf/10.1145/3732791) i tak maleje
   obciążenie czytelnika.
3. **Użyj, zmień, zbuduj.** Etap najpierw uruchamiamy w postaci gotowej, potem zmieniamy
   w nim jedno zachowanie i patrzymy, co się stanie, a dopiero na końcu piszemy własny
   fragment. Kolejność pochodzi z modelu
   [Use–Modify–Create](https://hal.science/hal-04739485v1/preview/ETS_26_3_12.pdf).

## Czego nie robimy

Nie wrzucamy pojęcia bez wyjaśnienia. Jeśli w rozdziale pojawia się liczba, nazwa albo skrót,
to znaczy, że czytelnik spotkał go wcześniej w tym samym przewodniku albo dostaje wyjaśnienie
w tym miejscu. Nazwy układów i typów kartridży pojawiają się dopiero wtedy, gdy wiadomo,
co taki układ robi i po co powstał. Żadnych liczb z cudzych pomiarów bez powodu i bez
wyjaśnienia, skąd się wzięły.

## Etapy

Każdy etap to katalog z kodem i rozdziałem. Po każdym etapie coś widać: na panelu,
na ekranie komputera albo w wyniku testu.

| Etap | Czego dotyczy | Co widać |
|---|---|---|
| 00 | Stanowisko pracy: kompilator, wgrywanie, pierwszy program | panel świeci jednym kolorem |
| 01 | Zmienne, funkcje, pętle i warunki na przykładzie kolorów | pasy kolorów |
| 02 | Wskaźniki i pamięć: czym jest bufor obrazu | własny obrazek z tablicy |
| 03 | Czym jest mikrokontroler: zegar, rejestry, czas | obraz zmienia się w czasie |
| 04 | Protokół panelu: co robi pięć przewodów i po co pasma | obraz wysyłany pasmami |
| 05 | Czym jest procesor: rejestry, stos, rozkazy | wynik testu na komputerze |
| 06 | Pamięć i autobus: skąd procesor bierze dane | program czytany z pamięci |
| 07 | Czym jest kartridż: nagłówek, pamięć programu i grafiki | nagłówek odczytany i wypisany |
| 08 | Układ obrazu: kafle, palety, tło | pierwszy prawdziwy obraz z kartridża |
| 09 | Duszki i kolejność rysowania | obraz z ruchomym obiektem |
| 10 | Czas: przerwania i klatki | obraz odświeżany w rytmie konsoli |
| 11 | Sterowanie: jak konsola czyta przyciski | własny ruch na ekranie |
| 12 | Przewijanie obrazu i podział ekranu | pasek statusu i ruchoma plansza |
| 13 | Kartridże większe niż pamięć: po co wymyślono przełączanie banków | inne gry działają |
| 14 | Najbardziej złożony kartridż z tych, które znamy | duża gra z tego układu |
| 15 | Dlaczego to chodzi wolno i co z tym zrobić | ta sama gra, wyraźnie szybsza |

Etapy 00-04 nie wymagają żadnej wiedzy o konsoli. Emulacja zaczyna się od etapu 05,
a kartridże od 07.

## Język

Tekst piszemy według zasad ze skilla `writing-polish`: strona czynna, konkret zamiast
ogólników, bez klisz i bez kalk z angielskiego, bez rozwlekłych wstępów. Każdy rozdział
odpowiada na pytanie czytelnika, a nie na pytanie autora. Terminy techniczne zostają
w brzmieniu przyjętym w dokumentacji (framebuffer, scanline), bo tłumaczenie ich na siłę
utrudnia czytanie źródeł, ale każde pierwsze użycie ma wyjaśnienie.

## Stan pracy

- [x] research metodyki i przegląd skilli językowych
- [x] refaktor repozytorium: standard stylu, `main.c`, `input.c`, `hal.c`, `font5x7`,
      przegląd narzędzi (przyrosty 1, 2 i 4 z `docs/STYLE.md`)
- [x] refaktor rdzeni emulacji: `cpu6502.c`, `ppu.c`, `mapper.c` (przyrost 3,
      komentarze i nazwy bez zmiany struktury, plik po pliku). Sprawdzone po
      scaleniu: 19/19 testów rdzenia, 262 144 i 1 966 080 sprawdzeń, oba buildy
      ARM bez ostrzeżeń, 0 różnych bajtów w trzech klatkach wzorcowych,
      `$030F = 3F`, harness różnicowy bez różnic na 5 mln instrukcji
- [x] wspólne budowanie etapów: `make STAGE=00 flash` (etapy z więcej niż jednym plikiem
      źródłowym mają własny `Makefile`, a etapy na komputer buduje `cc`)
- [x] etap 00: stanowisko pracy, kompilator, wgrywanie, pierwszy program
- [x] etap 01: język na kolorach (stałe, zmienne, tablice, funkcje, pętle, warunki)
- [x] etap 02: pamięć, adresy i wskaźniki (obraz powstaje w buforze)
- [x] etap 04: zegar, bufor obrazu i pasma (przeniesiony z numeru 01, gdy plan
      przesunął język na początek)
- [x] etap 03: czas i zegar
- [x] etapy 05-15: procesor, autobus, kartridż, obraz, duszki, przerwania,
      sterowanie, przewijanie, banki, złożony kartridż, wydajność
- [x] przegląd każdego etapu przez osobnego agenta (opis w REFLEKSJA.md)
- [x] wspólne budowanie obsługuje trzy rodzaje etapów: na płytkę, na komputer
      i takie, które przynoszą własny Makefile
- [x] poprawki zgłoszone przez recenzentów w etapach 00-04: obietnica zegara,
      kolejność czytania pliku, odniesienie rozmiaru bufora do pamięci,
      nieaktualne zdania w etapie 04
- [x] redakcja językowa wszystkich rozdziałów (cztery grupy etapów, osobny agent
      na każdą), razem z odsyłaczami do numerów etapów i spójnością terminów
- [ ] sprawdzenie etapów na panelu. To jedyna pozycja, której nie da się zamknąć
      bez przerwy w graniu: trzeba wgrać etap (na przykład 04), obejrzeć obraz,
      wgrać następny i na końcu wrócić z emulatorem na płytkę

## Do decyzji przed dalszą pracą

Trzy rzeczy zgłoszone przez recenzentów, których nie poprawili, bo wymagają decyzji
autora, a nie redakcji:

1. **Etap 13 czyta rejestr wyboru banku.** W tym emulatorze odczyt spod tego adresu
   zwraca pamięć stałego banku, a nie numer banku: rejestr jest tylko do zapisu.
   Rozdział już tego nie twierdzi, ale ćwiczenie polegające na odczytaniu numeru
   banku nie zadziała bez zmiany kodu emulatora.
2. **Etapy 03 i 04 oba wprowadzają zegar.** Etap 03 podnosi takt i tłumaczy arytmetykę,
   etap 04 ma własną kopię tego kodu. Dla czytelnika to powtórka; do rozstrzygnięcia,
   czy etap 04 ma się do 03 odwoływać, czy pokazywać pełny kod dla samodzielności.
3. **Etapy 14 i 15 mówią w pierwszej osobie**, choć reszta przewodnika zwraca się
   do czytelnika. Do ujednolicenia.

Osobno, z przeglądu rdzenia obrazu: kod ma kilka miejsc, w których różni się od
prawdziwego układu (kolejność duszków o tym samym priorytecie, porównywanie koloru
tła zamiast numeru palety przy trafieniu sprite 0, zawartość bufora odczytu palety).
Recenzent opisał je w komentarzach i nie zmieniał zachowania. To materiał na osobną
pracę nad wiernością emulacji, a nie na refaktor.
