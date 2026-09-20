# Etap 00: pierwszy program i świecący panel

Ten rozdział nie zakłada, że kiedykolwiek pisałeś program. Zakłada, że masz komputer,
dwie płytki i chęć sprawdzenia, jak to wszystko działa.

## Co to za sprzęt

Na stole leżą dwie płytki wciśnięte jedna w drugą.

Większa to **mikrokontroler**, czyli cały komputer na jednym układzie. Siedzi w nim procesor,
czyli układ wykonujący polecenia, pamięć programu, pamięć danych i dziesiątki wyprowadzeń,
czyli nóżek, do których można podłączyć inne układy. Nie ma na nim systemu operacyjnego ani
dysku. Program, który napiszesz, jest jedynym programem, jaki tam działa, i zaczyna się
od pierwszego rozkazu po włączeniu zasilania.

Mniejsza płytka to **panel**: prostokątna matryca 320 kolorowych punktów w poziomie i 240
w pionie. Te punkty nazywamy pikselami. Panel nie wie nic o twoim programie. Zachowuje się
jak pamięć, do której można tylko pisać: wysyłasz mu kolory, a on je pokazuje. Rozmawiamy
z nim przez pięć cienkich przewodów: jeden wyznacza rytm przesyłania, drugi niesie dane,
trzeci mówi, czy wysyłamy rozkaz, czy kolor, czwarty wskazuje, że to ten panel ma słuchać,
a piąty go restartuje.

Cel na dziś jest jeden: zapalić cały panel jednym kolorem. Brzmi skromnie, ale po drodze
przejdziesz przez wszystko, co w tej pracy będzie potrzebne: kompilator, wgrywanie programu
i pierwszy zapis do rejestru, czyli do miejsca, które steruje pracą układu.

## Czego potrzebujesz

- komputera z systemem macOS, Linux albo Windows,
- płytki NUCLEO-L476RG (ta większa) z wciśniętą na nią płytką rozszerzeń X-NUCLEO-GFX01M2
  (ta mniejsza, z panelem),
- kabla USB, którym połączysz płytkę z komputerem.

Kabel służy do dwóch rzeczy naraz: dostarcza zasilanie i łączy komputer z układem, który
wgrywa gotowy program do pamięci płytki. Ten układ nazywa się ST-Link i jest wlutowany
w większą płytkę, więc nie musisz kupować programatora.

## Kompilator, czyli program, który tłumaczy twój kod

Procesor rozumie wyłącznie liczby. Żeby nie pisać programów liczbami, piszemy je w języku C,
a osobny program tłumaczy go na to, co rozumie układ. Ten tłumacz to **kompilator**.
Potrzebujesz wersji, która tłumaczy kod na język procesora z twojej płytki.

Polecenia z tego rozdziału wpisujesz w terminalu, czyli w oknie, w którym zamiast klikać,
piszesz tekst i naciskasz Enter.

Na macOS kompilator instalujesz tak:

```bash
brew install --cask gcc-arm-embedded
```

Na Debianie i Ubuntu tak:

```bash
sudo apt install gcc-arm-none-eabi
```

Na Windowsie najprościej zainstalować środowisko MSYS2; kompilator dla Arm znajdziesz tam
w pakiecie `mingw-w64-ucrt-x86_64-arm-none-eabi-gcc`, a `st-flash` w pakiecie
`mingw-w64-ucrt-x86_64-stlink`.

Do wgrywania programu potrzebujesz narzędzia `st-flash`: na macOS dostaniesz je przez
`brew install stlink`, a na Debianie i Ubuntu przez `sudo apt install stlink-tools`.
Jeśli nie masz u siebie `brew`, zainstalujesz je ze strony Homebrew.

Do budowania potrzebujesz jeszcze programu `make`. Na macOS doinstalujesz go razem
z narzędziami deweloperskimi (`xcode-select --install`), na Debianie i Ubuntu razem
z pakietem `build-essential`, a w MSYS2 to osobny pakiet `make`.

Sprawdź, że kompilator odpowiada:

```bash
arm-none-eabi-gcc --version
```

Jeśli wypisze numer wersji, masz wszystko, czego potrzeba.

## Co znaczy „wgrać program"

Program, który napiszesz, musi trafić do pamięci, która nie kasuje się po odłączeniu
zasilania. Ta pamięć nazywa się **flash** i właśnie dlatego program zostaje na płytce
po wyjęciu kabla.

Program wgrywamy dwoma poleceniami: pierwsze wysyła gotowy program przez kabel, drugie
restartuje układ, żeby zaczął go wykonywać od początku. Plik `etap-00/etap00.bin` to gotowy
program, który za chwilę zbudujesz.

```bash
st-flash write etap-00/etap00.bin 0x08000000
st-flash reset
```

Liczba `0x08000000` to adres, czyli numer, od którego zaczyna się pamięć programu w tym
układzie. Ta sama liczba wróci w poleceniach, które wgrywają program.

## Pierwszy program

Kod etapu leży w tym katalogu, w pliku `main.c`. Żeby go zbudować, otwórz terminal
w katalogu `tutorial-pl`, o poziom wyżej, i wpisz:

```bash
make STAGE=00 flash
```

`make` to program, który wykonuje zapisane wcześniej polecenia budowania, żeby nie trzeba
było przepisywać ich za każdym razem. To jedno polecenie robi trzy rzeczy: kompiluje kod,
zamienia go na postać, którą można wysłać do płytki, i wgrywa ją razem z restartem.

Jeśli wolisz zobaczyć każdy krok osobno, to samo wygląda tak:

```bash
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 -Wall -Wextra -nostartfiles \
    -T ../src/linker.ld -o etap-00/etap00.elf etap-00/main.c ../src/startup_l476.s
arm-none-eabi-objcopy -O binary etap-00/etap00.elf etap-00/etap00.bin
st-flash write etap-00/etap00.bin 0x08000000
st-flash reset
```

Dwie rzeczy w pierwszym poleceniu są pożyczone z gotowego emulatora, czyli z programu, który
udaje konsolę do gier. Ten emulator leży w tym samym repozytorium, czyli w katalogu z całym
kodem. Plik `startup_l476.s` wykonuje się zaraz po włączeniu zasilania, przygotowuje pamięć
i przekazuje sterowanie do twojego `main`, a `linker.ld` mówi kompilatorowi, gdzie w pamięci
ma wylądować kod. Do pliku startowego wrócimy w etapie 10, gdy program zacznie reagować
na sygnały zegara, które przerywają mu pracę.

## Co program robi

Zaglądnij do `main.c`. Rejestr to miejsce w układzie, które ma swój adres. Zapisanie liczby
pod ten adres zmienia zachowanie układu. W kodzie zobaczysz zapisy postaci `GPIOA_BSRR = ...`:
piszesz pod adres, a układ reaguje.

Kolejność poleceń jest zawsze ta sama i od niej zależy, czy zobaczysz cokolwiek:

1. **Włączamy zegary.** Każda część układu ma swój zegar, czyli rytm, w którym pracuje.
   Dopóki go nie włączymy, zapisy do jej rejestrów nic nie robią. Program, który o tym
   zapomni, wygląda, jakby się zawiesił, choć nic złego się nie dzieje.
2. **Ustawiamy wyprowadzenia.** Mówimy układowi, które wyprowadzenia mają wysyłać dane
   do panelu, a które mają nim sterować.
3. **Resetujemy panel.** Panel potrzebuje chwili, żeby się obudzić, więc program przytrzymuje
   przez moment jego wyprowadzenie resetu, puszcza je i czeka.
4. **Wysyłamy rozkazy startowe.** Mówimy panelowi, w jakim formacie będziemy podawać kolory
   i jak ma ustawić obraz.
5. **Wysyłamy kolory.** Panel dostaje informację, jaki prostokąt wypełniamy, a potem same
   kolory, dwa bajty na każdy piksel. Bajt to osiem bitów, a bit jest jak przełącznik:
   włączony albo wyłączony.

## Co powinieneś zobaczyć

Cały panel w jednym kolorze. Wypełnianie trwa zauważalnie długo, bo na tym etapie
mikrokontroler pracuje na zegarze startowym, czyli w wolnym rytmie, w jakim budzi się
po włączeniu zasilania. To celowe: w etapie 03 podniesiemy ten rytm dwadzieścia razy
i zobaczysz różnicę gołym okiem.

Jeśli ekran zostaje biały, szary albo czarny, sprawdź po kolei trzy rzeczy. Czy przewody
są wciśnięte do końca i czy płytka rozszerzeń siedzi w podstawie. Czy kompilacja przeszła
bez błędów, bo `make` wypisuje każdy krok. I czy płytka naprawdę zaczęła nowy program:
`make STAGE=00 flash` restartuje ją sam, ale jeśli wgrywałeś program ręcznie, trzeba było
po `st-flash write` wykonać jeszcze `st-flash reset`.

## Ćwiczenia

Zmień kolor w pliku `main.c`. Niebieski to `0x001F`, czerwony `0xF800`, a zielony `0x07E0`;
podmieniaj liczbę i wgrywaj program od nowa. Kolor zapisujemy jako jedną liczbę, w której
zaszyte są trzy składniki: czerwony, zielony i niebieski. Liczby z `0x` na początku są
szesnastkowe, czyli zapisane w systemie o podstawie szesnaście. Na razie nie musisz
wiedzieć dokładnie jak.

Zwiększ dwukrotnie opóźnienie po resecie panelu, a potem zmniejsz je mocno. W `main.c` stoi
tam `delay(1200000)`; to pierwsze z dwóch tak długich opóźnień, zaraz po zdjęciu resetu.
Sprawdź, co się stanie przy tysiącu. Jeśli obraz przestanie się pojawiać, właśnie
zobaczyłeś, po co było to opóźnienie.

## Co dalej

W etapie 01 zajmiemy się językiem: stałymi, zmiennymi, tablicami, funkcjami, pętlami
i warunkami. Wszystko na kolorach, które już umiesz wyświetlić, więc każde nowe pojęcie
będzie miało natychmiast widoczny skutek na panelu.
