# Etap 0: niech panel się zaświeci

Zanim zbudujemy cokolwiek, co przypomina konsolę, musimy nauczyć się rozmawiać z wyświetlaczem.
Ten etap robi jedną rzecz: wypełnia cały ekran jednym kolorem. Jeśli to zobaczysz, znaczy że
płytka żyje, przewody są dobrze wpięte, a panel rozumie, co do niego mówimy.

## Co masz na stole

Dwie płytki wciśnięte jedna w drugą: NUCLEO-L476RG z mikrokontrolerem i shield
X-NUCLEO-GFX01M2 z panelem dotykowym... nie, bez dotyku. Panel to ST7789, matryca
320 na 240 pikseli, podłączona do mikrokontrolera pięcioma przewodami:

| pin mikrokontrolera | po co |
|---|---|
| PA5 | zegar SPI, czyli takt, w rytm którego idą bity |
| PA7 | dane do panelu |
| PA9 | chip select, wybór układu (my nim sterujemy ręcznie) |
| PB10 | data/command, mówi panelowi, czy bajt jest rozkazem, czy pikselem |
| PA1 | reset panelu |

Zasilanie i masa idą przez złącza Arduino, więc nie ma ich na tej liście.

## Jak panel myśli

Panel nie ma magistrali adresowej ani pamięci, do której mógłbyś zajrzeć. Zachowuje się
jak rejestr przesuwny z dwoma dodatkowymi wejściami. Wystawiasz bajt na SPI, a panel patrzy
na dwa piny obok: jeśli `CS` jest niski (czyli wybraliśmy ten układ), a `DC` jest niski,
bajt jest rozkazem. Jeśli `DC` jest wysoki, bajt jest kolorem piksela. Nic więcej nie ma
do zrozumienia, cała reszta to kolejność i czasy.

Dlatego `CS` i `DC` sterujemy sami, zwykłymi zapisami do rejestru `BSRR`. Sprzętowy SPI
w tym mikrokontrolerze potrafi zarządzać `CS` sam, ale wtedy nie widzimy, co się dzieje,
a na tym etapie chcemy widzieć wszystko.

## Co robi program

Kolejność jest zawsze ta sama i od niej zależy, czy zobaczysz cokolwiek:

1. **Zegary.** Najpierw włączamy taktowanie dla portów GPIO i dla SPI1. Bez tego zapisy
   do rejestrów nie robią nic i program wygląda, jakby się zawiesił.
2. **Piny.** PA5 i PA7 ustawiamy jako wyjścia alternatywne SPI (funkcja 5), PA9, PB10 i PA1
   jako zwykłe wyjścia.
3. **Reset panelu.** Przytrzymujemy PA1 nisko, potem puszczamy. Panel potrzebuje na to
   około 120 milisekund, więc jeśli pominiesz opóźnienie, obraz się nie pojawi.
4. **Rozkazy startowe.** Ustawiamy format koloru na 16 bitów na piksel, orientację na poziomą,
   wyprowadzamy panel ze snu i włączamy wyświetlanie.
5. **Okno i piksele.** Mówimy panelowi, jaki prostokąt będziemy wypełniać (`CASET` i `RASET`),
   a potem wysyłamy `RAMWR` i szesnaście tysięcy... dokładnie 76 800 kolorów, dwa bajty każdy.

Kolor zapisujemy w formacie RGB565: pięć bitów czerwonego, sześć zielonego, pięć niebieskiego.
Zielonego jest więcej, bo ludzkie oko najłatwiej rozróżnia jego odcienie. Niebieski z tego
etapu to `0x001F`.

## Zbuduj i wgraj

Z katalogu `tutorial-pl/etap-00`:

```bash
# z katalogu tutorial-pl:
make STAGE=00 flash
```

To samo ręcznie, jeśli chcesz zobaczyć każdy krok:

```bash
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 -nostartfiles \
    -T ../../src/linker.ld ../../src/startup_l476.s main.c -o etap00.elf
arm-none-eabi-objcopy -O binary etap00.elf etap00.bin
st-flash write etap00.bin 0x08000000
st-flash reset
```

Plik `startup_l476.s` i skrypt linkera pożyczamy z emulatora: pierwszy ustawia wektor
przerwań i skacze do `main`, drugi mówi linkerowi, gdzie w pamięci ma wylądować kod.
Oba omówimy dokładniej w etapie 2, kiedy będzie już trzeba obsługiwać przerwania.

## Co powinieneś zobaczyć

Cały panel w kolorze niebieskim. Wypełnianie trwa zauważalnie długo, bo mikrokontroler
pracuje na zegarze startowym około 4 MHz, a SPI dzieli go na pół. To celowe: w etapie 1
podniesiemy zegar do 80 MHz i zobaczysz różnicę gołym okiem.

Jeśli ekran zostaje biały albo szary, sprawdź po kolei: czy `CS` jest wysoki w spoczynku
(panel wybiera się stanem niskim), czy `DC` zmienia się między rozkazem a danymi i czy
opóźnienie po resecie jest naprawdę odczekane. Prawie każdy początkujący błąd na tym
etapie to jedna z tych trzech rzeczy.

## Ćwiczenia

Zmień kolor na czerwony, potem na zielony. Zobacz, jak wygląda `0xF800` i `0x07E0`
w formacie RGB565.

Potem zmień wartość wysyłaną rozkazem `MADCTL` z `0x60` na `0xA0` i zastanów się, dlaczego
obraz jest nadal pełny, choć panel jest obrócony o 180 stopni. To nie pomyłka: przy jednolitym
kolorze obrót jest niewidoczny. Zauważysz go dopiero wtedy, gdy na ekranie pojawi się treść
z orientacją, czyli w etapie 1.

## Czego ten etap jeszcze nie ma

Bufora obrazu. Kolor leci wprost na panel, więc nie ma czego przerysowywać ani porównywać.
Emulator potrzebuje bufora 256 na 240 bajtów i wysyłania go pasmami, żeby emulacja nie
czekała na koniec transmisji. Tym zajmiemy się w etapie 1.
