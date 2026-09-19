# Przegląd etapów, refleksja i plan reszty

**Stan po dopisaniu reszty.** Wszystkie etapy 00-15 istnieją: kod i rozdziały, każdy
sprawdzony kompilacją bez ostrzeżeń, a etapy na komputer dodatkowo uruchomieniem.
Poniższa refleksja powstała, gdy istniały tylko 00-04, i zostawiam ją jako zapis tego,
co wtedy znalazłem; uwagi w niej dotyczące braków są już nieaktualne, a trzy poprawki
w etapach 00-02 nadal czekają. Specyfikacje etapów 05-15 też zostawiam: opisują, co te
etapy miały zawierać, więc można je porównać z tym, co naprawdę powstało.

Ten plik jest notatką z czytania własnego tekstu od początku, razem z planem etapów, których
kodu jeszcze nie ma. Ma odpowiadać na dwa pytania: czy to, co jest, da się zrozumieć bez
wiedzy wcześniejszej, i czy trudność rośnie stopniowo. Piszę tu także to, co znalazłem
nie tak, bo notatka, która chwali, jest bezużyteczna.

## Co przeczytałem i co z tego wynika

**Etap 00 (stanowisko pracy, kompilator, wgrywanie, pierwszy program).** Czyta się dobrze
i nie zakłada niczego. Pierwsze dwa akapity mówią, co leży na stole, bez słowa o konsoli.
Znalazłem jedną rzecz do poprawy: rozdział obiecuje, że „w etapie 03 podniesiemy zegar",
a etap 03 jeszcze nie istnieje. Obietnica jest prawdziwa, ale musi doczekać się swojego
etapu, inaczej czytelnik trafi w pustkę.

**Etap 01 (język).** Kolejność pojęć jest dobra: stała, zmienna, tablica, funkcja, pętla,
warunek. Każde pojęcie kończy się czymś, co widać. Mankament: rozdział pokazuje pełny kod
funkcji `fill_rectangle`, ale nie mówi, gdzie w pliku ona leży ani w jakiej kolejności
czytać plik. Czytelnik może nie zauważyć, że `main` jest na końcu i że to od niego zaczyna
się wykonanie programu.

**Etap 02 (pamięć i wskaźniki).** Najtrudniejszy z dotychczasowych i napisany najostrożniej:
adres, wskaźnik, różnica między `*pixels = 5` a `pixels++`, tablica jako adres. Dwie funkcje
robiące to samo na dwa sposoby to dobry pomysł, bo pokazuje, że to jeden mechanizm.
Mankament: brakuje zdania, ile pamięci zajmuje bufor obrazu w porównaniu z całym dostępnym
RAM-em. Liczba 76 800 bajtów jest, ale bez odniesienia do 96 kilobajtów czytelnik nie poczuje,
że to dużo.

**Etap 04 (zegar, bufor, pasma).** Treść jest dobra, ale kolejność w planie jest zła:
etap używa bufora obrazu i wskaźników, więc nie może stać przed etapem 02. Plan przewiduje
go po 03, i tak zostanie.

**Czego brakuje najbardziej.** Etapu 03 (czas i zegar), bo bez niego nie da się pokazać,
że program może działać sam, a bez tego etap 04 (pasma) nie ma sensu: pasma istnieją
po to, żeby coś robić w czasie transmisji. To następny etap do napisania.

## Etapy, których jeszcze nie ma

Każdy opis mówi, czego uczy, co zawiera kod i co czytelnik widzi. Liczby w nawiasach to
etapy, na których dany etap się opiera.

**03. Czas i zegar** (po 01, 02). Uczy: po co mikrokontrolerowi zegar, czym są rejestry,
jak zrobić pętlę, która działa w nieskończoność, i jak odmierzać czas bez liczenia obrotów
pętli. Kod: włączenie PLL i podniesienie taktu z kilku do kilkudziesięciu megaherców,
pętla główna, opóźnienie w milisekundach. Czytelnik widzi: obraz zmieniający się w czasie,
na przykład przesuwający się pas.

**05. Czym jest procesor** (po 04). Uczy: procesor to maszyna o kilku rejestrach, która
wykonuje rozkazy jeden po drugim; czym jest stos; dlaczego potrzebny jest licznik rozkazów.
Kod: szkielet emulacji, na razie na komputerze, nie na płytce: struktura z rejestrami
i kilkanaście rozkazów, uruchamiane z prostego testu. Czytelnik widzi: wynik testu na
komputerze, na przykład liczbę wykonanych rozkazów i ich wpływ na rejestry.

**06. Pamięć i autobus** (po 05). Uczy: procesor nie ma danych w sobie, tylko sięga po nie
pod adresy; czym jest mapa pamięci i czym się różni pamięć od rejestrów sprzętowych.
Kod: funkcje czytania i pisania pod adres, tablica pamięci, pierwszy program wykonywany
z tej pamięci. Czytelnik widzi: program, który sam siebie modyfikuje, i wynik na komputerze.

**07. Czym jest kartridż** (po 06). Uczy: gra to plik z nagłówkiem i dwiema częściami,
programem i grafiką; czym jest bank pamięci. Kod: odczyt nagłówka pliku, wypisanie jego
pól. Czytelnik widzi: wypisane pola nagłówka prawdziwego pliku z grą, na przykład rozmiary
pamięci programu i grafiki.

**08. Układ obrazu: kafle i palety** (po 07). Uczy: obraz konsoli nie jest rysowany piksel
po pikselu, tylko składany z małych kafelków 8 na 8; czym jest paleta i czym różni się
od kolorów. Kod: odczyt pamięci grafiki z kartridża i zamiana kafli na piksele, na razie
bez ruchu. Czytelnik widzi: pierwszy prawdziwy obraz z gry na panelu.

**09. Duszki** (po 08). Uczy: obraz ruchomy to osobna lista małych obrazków z pozycjami;
dlaczego kolejność rysowania ma znaczenie. Kod: lista duszków, ich pozycje, pierwszeństwo
tła nad duszkiem. Czytelnik widzi: obraz z ruchomym obiektem, który można przesuwać.

**10. Czas w konsoli: przerwania i klatki** (po 09). Uczy: czym jest przerwanie i po co
konsola przerwała pracę procesora sześćdziesiąt razy na sekundę. Kod: obsługa przerwania,
odliczanie klatek, oddzielenie rysowania od logiki. Czytelnik widzi: obraz odświeżany
w równym rytmie, bez migotania.

**11. Sterowanie** (po 10). Uczy: jak konsola czyta przyciski po jednym drucie; czym jest
rejestr przesuwny. Kod: odczyt przycisków z płytki i podanie ich do emulowanego programu.
Czytelnik widzi: własny ruch na ekranie, po raz pierwszy ze swoim udziałem.

**12. Przewijanie i podział ekranu** (po 11). Uczy: jak gra przesuwa obraz, nie przerysowując
go; jak jeden ekran pokazuje dwie różne rzeczy naraz. Kod: rejestry przewijania, oczekiwanie
na odpowiedni moment w obrazie. Czytelnik widzi: pasek statusu stojący w miejscu i planszę
przewijającą się pod nim.

**13. Kartridże większe niż pamięć** (po 12). Uczy: dlaczego wymyślono przełączanie pamięci
i po co gra podmienia fragmenty programu w trakcie działania. Kod: obsługa kartridża, który
sam decyduje, co ma pod danym adresem. Czytelnik widzi: działającą grę, która nie mieści
się w pamięci procesora.

**14. Najbardziej złożony kartridż** (po 13). Uczy: jak jeden układ potrafi dzielić ekran,
zmieniać palety w trakcie linii i odliczać linie obrazu. Kod: obsługa tego układu i jego
przerwania. Czytelnik widzi: dużą grę, która na tym układzie została wydana.

**15. Dlaczego to chodzi wolno** (po 14). Uczy: jak mierzyć czas programu, zamiast zgadywać;
gdzie znika czas w emulatorze i co się opłaca, a co nie. Kod: licznik cykli, pomiar etapów
klatki, dwie albo trzy zmiany wybrane na podstawie pomiaru. Czytelnik widzi: tę samą grę,
wyraźnie szybszą, i tabelę, która mówi, skąd wzięła się różnica.

## Wnioski dla dalszej pracy

Kolejność jest dobra i nie wymaga zmiany: 00, 01, 02, 03, 04, potem emulacja od 05.
Do poprawy w istniejących rozdziałach zostały dwie rzeczy: obietnica z etapu 00 o podniesieniu
zegara musi doczekać się etapu 03, a etap 01 powinien powiedzieć, w jakiej kolejności czytać
plik i od czego zaczyna się wykonanie programu. Trzecia rzecz to brak odniesienia w etapie 02
między rozmiarem bufora a rozmiarem pamięci, którą dysponuje mikrokontroler.

Żadnego z tych trzech braków nie da się usunąć bez napisania etapu 03, więc to jest następna
praca: kod i rozdział etapu 03, a po nim poprawki w 00, 01 i 02.
