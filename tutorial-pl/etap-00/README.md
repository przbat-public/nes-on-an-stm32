# Etap 00: pierwszy program i świecący panel

Ten rozdział nie zakłada, że kiedykolwiek pisałeś program. Zakłada, że masz komputer,
dwie płytki i chęć sprawdzenia, jak to wszystko działa.

## Co to za sprzęt

Na stole leżą dwie płytki wciśnięte jedna w drugą.

Większa to **mikrokontroler**, czyli cały komputer na jednym układzie: procesor, pamięć
programu, pamięć danych i dziesiątki wyprowadzeń, do których można podłączyć inne układy.
Nie ma na nim systemu operacyjnego ani dysku. Program, który napiszesz, jest jedynym
programem, jaki tam działa, i zaczyna się od pierwszej instrukcji po włączeniu zasilania.

Mniejsza płytka to **panel**: prostokątna matryca 320 na 240 małych, kolorowych punktów,
które nazywamy pikselami. Panel nie wie nic o twoim programie. Zachowuje się jak pamięć,
do której można tylko pisać: wysyłasz mu kolory, a on je pokazuje. Rozmawiamy z nim przez
pięć cienkich przewodów, o których będzie mowa w tym rozdziale.

Cel na dziś jest jeden: zapalić cały panel jednym kolorem. Brzmi skromnie, ale po drodze
przejdziesz przez wszystko, co w tej pracy będzie potrzebne: kompilator, wgrywanie programu
i pierwszy zapis do rejestru sprzętowego.

## Czego potrzebujesz

- komputera z systemem macOS, Linux albo Windows,
- płytki NUCLEO-L476RG z wciśniętą na nią płytką rozszerzeń X-NUCLEO-GFX01M2,
- kabla USB, którym połączysz płytkę z komputerem.

Kabel służy do dwóch rzeczy naraz: dostarcza zasilanie i łączy komputer z układem, który
wgra gotowy program do pamięci płytki. Ten układ nazywa się ST-Link i jest wlutowany
w większą płytkę, więc nie musisz kupować programatora.

## Kompilator, czyli program, który tłumaczy twój kod

Procesor rozumie wyłącznie liczby. Żeby nie pisać programów liczbami, piszemy je w języku C,
a osobny program tłumaczy go na to, co rozumie układ. Ten tłumacz to **kompilator**.
Potrzebujesz wersji, która produkuje kod dla procesorów Arm, bo taki jest w twojej płytce.

Na macOS instaluje się go tak:

```bash
brew install --cask gcc-arm-embedded
```

Na Debianie i Ubuntu tak:

```bash
sudo apt install gcc-arm-none-eabi
```

Na Windowsie najprościej pobrać pakiet ze strony producenta procesorów Arm. Do wgrywania
programu potrzebujesz jeszcze narzędzia `st-flash`; na macOS dostaniesz je przez
`brew install stlink`, na Debianie i Ubuntu przez `sudo apt install stlink-tools`.

Sprawdź, że kompilator odpowiada:

```bash
arm-none-eabi-gcc --version
```

Jeśli wypisze numer wersji, masz wszystko, czego potrzeba.

## Co znaczy „wgrać program"

Program, który napiszesz, musi trafić do pamięci płytki, takiej, która nie kasuje się
po odłączeniu zasilania. Ta pamięć nazywa się **flash** i właśnie dlatego program zostaje
na płytce po wyjęciu kabla.

Wgrywanie robi się dwiema komendami: pierwsza wysyła gotowy program przez kabel,
druga restartuje układ, żeby zaczął go wykonywać od początku.

```bash
st-flash write etap00.bin 0x08000000
st-flash reset
```

Liczba `0x08000000` to adres, od którego zaczyna się pamięć programu w tym układzie.
Zapamiętasz ją bez wysiłku, bo spotkasz ją jeszcze w skryptach.

## Pierwszy program

Kod etapu leży w tym katalogu, w pliku `main.c`. Żeby go zbudować, wystarczy jedno polecenie:

```bash
make STAGE=00 flash
```

`make` to program, który wykonuje zapisane wcześniej polecenia budowania, żeby nie trzeba
było przepisywać ich za każdym razem. To jedno polecenie robi trzy rzeczy: kompiluje kod,
zamienia go na postać, którą wysyła się do płytki, i wgrywa ją razem z restartem.

Jeśli wolisz zobaczyć każdy krok osobno, to samo wygląda tak:

```bash
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 -nostartfiles \
    -T ../src/linker.ld ../src/startup_l476.s main.c -o etap00.elf
arm-none-eabi-objcopy -O binary etap00.elf etap00.bin
st-flash write etap00.bin 0x08000000
st-flash reset
```

Dwie rzeczy w pierwszym poleceniu są pożyczone z gotowego emulatora, który leży w tym samym
repozytorium: plik `startup_l476.s` ustawia wektor przerwań i skacze do twojego `main`,
a `linker.ld` mówi kompilatorowi, gdzie w pamięci ma wylądować kod. Oba omówimy dokładnie
wtedy, gdy będzie trzeba obsługiwać przerwania.

## Co program robi

Zaglądnij do `main.c`. Kolejność jest zawsze ta sama i od niej zależy, czy zobaczysz cokolwiek:

1. **Włączamy taktowanie.** Każda część układu ma swój zegar i dopóki nie zostanie włączony,
   zapisy do jego rejestrów nic nie robią. Program, który o tym zapomni, wygląda, jakby się
   zawiesił, choć nic złego się nie dzieje.
2. **Ustawiamy wyprowadzenia.** Mówimy układowi, które nóżki mają wysyłać dane do panelu,
   a które mają nim sterować.
3. **Resetujemy panel.** Panel potrzebuje chwili, żeby się obudzić, więc przytrzymujemy
   jedną nóżkę w stanie niskim, puszczamy ją i czekamy.
4. **Wysyłamy rozkazy startowe.** Mówimy panelowi, w jakim formacie będziemy podawać kolory
   i jak ma ustawić obraz.
5. **Wysyłamy kolory.** Panel dostaje informację, jaki prostokąt wypełniamy, a potem same
   kolory, dwa bajty na każdy piksel.

Rejestr to miejsce w układzie, które ma swój adres. Zapisanie liczby pod ten adres zmienia
jego zachowanie. W kodzie zobaczysz zapisy postaci `GPIOA_BSRR = ...` i to jest właśnie cała
magia: piszesz pod adres, układ reaguje.

## Co powinieneś zobaczyć

Cały panel w jednym kolorze. Wypełnianie trwa zauważalnie długo, bo na tym etapie
mikrokontroler pracuje na zegarze startowym, kilkanaście razy wolniejszym od tego, na którym
będzie pracował później. To celowe. W etapie 03 podniesiemy zegar i zobaczysz różnicę
gołym okiem.

Jeśli ekran zostaje biały, szary albo czarny, sprawdź po kolei trzy rzeczy. Czy przewody
są wciśnięte do końca i czy płytka rozszerzeń siedzi w podstawie. Czy kompilacja przeszła
bez błędów, bo `make` wypisuje każdy krok. I czy po wgraniu wykonałeś `st-flash reset`,
bo bez tego płytka dalej wykonuje poprzedni program.

## Ćwiczenia

Zmień kolor w pliku `main.c` na czerwony, potem na zielony, i za każdym razem wgraj program
od nowa. Kolor zapisujemy jako liczbę szesnastkową, w której jedne bity opisują czerwony,
inne zielony, a ostatnie niebieski. Nie musisz jeszcze wiedzieć dokładnie jak; wystarczy,
że podmienisz liczbę i zobaczysz efekt.

Zwiększ opóźnienie po resecie panelu, na przykład dwa razy, i sprawdź, czy coś się zmieniło.
Potem zmniejsz je mocno, do kilkuset. Jeśli obraz przestanie się pojawiać, właśnie
zobaczyłeś, po co było to opóźnienie.

## Co dalej

W etapie 01 zostawimy jeden kolor i zajmiemy się językiem: zmiennymi, funkcjami, pętlami
i warunkami. Wszystko na kolorach, które już umiesz wyświetlić, więc każde nowe pojęcie
będzie miało natychmiast widoczny skutek na panelu.
