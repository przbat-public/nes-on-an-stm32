# Emulator NES na STM32: tutorial od zera

Ten katalog to tutorial pisany po polsku dla ucznia technikum elektronicznego.
Kod i komentarze w całym repozytorium zostają po angielsku, tekst nauki jest polski.

Repozytorium, w którym leży ten katalog, zawiera **działający emulator**: cztery mappery
(NROM, MMC1, UxROM, MMC3, MMC5), obsługę pada, obraz na panelu ST7789 i pomiary wydajności.
Tutorial nie streszcza tego kodu. Prowadzi do niego własną, prostą drogą, a gotowy emulator
służy jako punkt odniesienia w ostatnich rozdziałach: pokazuje, co trzeba było zrobić,
żeby gra chodziła 47 razy na sekundę, a nie 17.

## Dla kogo

Czytelnik zna podstawy C (zmienne, wskaźniki, funkcje, przerwania) i potrafi wgrać
program na płytkę. Nie musi znać NES-a ani budowy procesora 6502. Każdy rozdział
tłumaczy nowy kawałek sprzętu albo nowy mechanizm konsoli, i kończy się czymś,
co widać na ekranie.

## Jak uczymy

Trzy zasady, każda z konkretnego powodu:

1. **Tutorial prowadzi, nie opisuje.** Najpierw robimy, potem rozumiemy. Rozdział ma
   zawsze jeden wynik: działający program i obraz, który można sprawdzić okiem.
   Podział na typy dokumentacji pochodzi z metodyki [Diátaxis](https://diataxis.fr/pl/start-here/):
   ten katalog jest tutorialem, a `docs/ARCHITECTURE.md` i `docs/PERFORMANCE.md`
   w repozytorium pełnią rolę wyjaśnienia i referencji.
2. **Przykład rozpracowany zamiast opisu.** Każdy etap to kompletny, uruchamialny
   program, a wyjaśnienie stoi obok kodu, nie w osobnym rozdziale. Tak działa
   [efekt przykładu rozpracowanego](https://dl.acm.org/doi/pdf/10.1145/3732791)
   i tak maleje obciążenie poznawcze czytelnika.
3. **Użyj, zmień, zbuduj.** Etap najpierw uruchamiamy w postaci gotowej, potem
   zmieniamy jedno zachowanie i patrzymy, co się stanie, a dopiero na końcu piszemy
   własny fragment. Kolejność pochodzi z modelu
   [Use–Modify–Create](https://hal.science/hal-04739485v1/preview/ETS_26_3_12.pdf).

## Etapy

Każdy etap to katalog z kodem i rozdziałem tekstu. Po każdym etapie uczeń widzi
konkretny efekt na panelu, więc wie, czy idzie dobrze.

| Etap | Czego dotyczy | Co widać na panelu |
|---|---|---|
| 0 | Płytka, panel, SPI, pierwszy piksel | kolorowy prostokąt |
| 1 | Obraz: framebuffer i wysyłanie pasmami | wzór testowy |
| 2 | Rdzeń 6502: rejestry, stos, rozkazy | nic, wynik sprawdzamy na hoście |
| 3 | Autobus pamięci i cartridge iNES | nagłówek ROM-u na ekranie |
| 4 | PPU: kafle, palety, tło | pierwszy obraz z konsoli |
| 5 | PPU: duszki i priorytety | obraz z ruchomym obiektem |
| 6 | Przerwanie NMI i pętla ramki | obraz odświeżany 60 razy na sekundę |
| 7 | Sterowanie: pad i protokół `$4016` | własny ruch na ekranie |
| 8 | Przewijanie, sprite zero, podział ekranu | pasek statusu i ruchoma plansza |
| 9 | Mappery: MMC1 i UxROM | inne cartridge'e |
| 10 | MMC3 i przerwanie od licznika linii | gry z podziałem ekranu |
| 11 | MMC5 | Castlevania III |
| 12 | Wydajność: gdzie ucieka czas ramki | te same gry, trzy razy szybciej |

## Język

Tekst piszemy według zasad ze skilla `writing-polish`: strona czynna, konkret
zamiast ogólników, bez klisz i bez kalk z angielskiego, bez rozwlekłych wstępów.
Każdy rozdział odpowiada na pytanie czytelnika, a nie na pytanie autora.
Terminy techniczne zostają w brzmieniu przyjętym w dokumentacji (framebuffer,
scanline, mapper), bo tłumaczenie ich na siłę utrudnia czytanie źródeł.

## Stan pracy

- [x] research metodyki i przegląd skilli językowych
- [ ] refaktor repozytorium pod czytelność, bez straty wydajności i RAM-u
- [ ] kod dydaktyczny: etapy 0-12, każdy uruchamialny i sprawdzony na płytce
- [ ] tekst rozdziałów po polsku
- [ ] redakcja językowa i przegląd całości
