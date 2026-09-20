# Etap 11: sterowanie, czyli jak konsola czyta przyciski

Do tej pory obraz robił to, co postanowił program. Ten etap daje programowi pierwszą
rzecz, na którą masz wpływ: drążek obok ekranu i niebieski przycisk na większej płytce.
Ale nie tak, jak byłoby wygodnie, tylko tak, jak robi to konsola: jeden drut, po którym
osiem bitów wędruje jedno za drugim.

## Przycisk to przerwa w przewodzie

Przycisk to zwykły wyłącznik: jedna końcówka do nóżki układu, druga do masy, czyli zera.
Wciśnięty zwiera nóżkę z masą, puszczony nie robi nic.

To „nic" jest kłopotliwe: nóżka bez podłączenia zbiera zakłócenia, więc program raz
odczyta jedynkę, a raz zero. Dlatego do nóżki dołącza się rezystor **podciągający**, który
trzyma ją na poziomie zasilania. Nasz mikrokontroler ma taki rezystor w środku, a włącza go
jedna linia w `input_init`. Puszczony przycisk czytamy wtedy jako 1, wciśnięty jako 0.

Drążek ma cztery styki, niebieski przycisk siedzi osobno, czyli razem pięć wejść i pięć
bitów. Nóżki mają nazwy z litery portu, czyli grupy wyprowadzeń, i numeru w tej grupie:
PB4 to czwarta nóżka portu B, a PC0 to nóżka zerowa portu C. Nóżki PA0 nie ma na tej
liście: płytka łączy ją na stałe z masą, więc program odczytałby ją zawsze jako wciśniętą.

## Osiem przycisków po jednym drucie

Pad konsoli, czyli kontroler z przyciskami, ma osiem wejść: cztery kierunki, przyciski
A i B oraz Select i Start. Osiem osobnych przewodów znaczyłoby grubszy kabel, większą
wtyczkę i osiem nóżek zajętych w konsoli. Drożej, a nie lepiej.

Elektronika przeniosła się więc do pada, a do konsoli wracają trzy przewody sygnałowe:
latch (zatrzask), zegar i dane. W środku pada siedzi **rejestr przesuwny**: rząd ośmiu
komórek, z których każda trzyma jeden bit. W oryginalnym padzie to jeden układ scalony,
oznaczony numerem 4021. Rząd umie dwie rzeczy: skopiować stan wszystkich przycisków naraz,
kiedy latch jest podniesiony, i przesunąć się o jedno miejsce, kiedy konsola szarpnie
zegarem. Wtedy skrajna komórka wypada na przewód danych.

Gra czyta więc osiem kolejnych bitów z jednego drutu, zawsze w tej samej kolejności:
A, B, Select, Start, góra, dół, lewo, prawo. Program gry widzi to jako jeden adres: `$4016`
w dokumentacji konsoli, czyli `0x4016` w naszym zapisie. Zapis pod ten adres podnosi latch,
a odczyt daje jeden impuls zegara. Nasz emulowany program to na razie kilkanaście linii C
i nie zna tego adresu: woła `read_pad` i dostaje ten sam bajt.

## Jak to wygląda w kodzie

`read_pad_pins` czyta pięć nóżek i składa z nich jeden bajt: wciśnięty przycisk ustawia
swój bit. `pad_write_latch` i `pad_read_bit` udają rejestr przesuwny — latch kopiuje stan
przycisków do komórek, a każdy odczyt oddaje najniższy bit i przesuwa rząd. W prawdziwym
padzie ten rejestr istnieje, na naszej płytce go nie ma, więc odtwarzamy go w programie.

Latch jest po to, żeby gra widziała stan z jednej chwili: kiedy go podnosisz, wszystkie
przyciski trafiają do komórek naraz. Wciśnięcie, które przyjdzie później, poczeka do
następnej klatki, a takie, które zacznie się i skończy między dwoma odczytami, przepada.
`read_pad` robi to, co gra na początku klatki: podnosi latch, opuszcza go i czyta osiem
bitów, wkładając każdy na swoje miejsce.

Pętla główna czyta się jak zdanie: `emulated_frame` rozgrywa jedną klatkę i zwraca bajt
z pada, `draw_frame` rysuje to, co z niej wynikło, a `push_picture` wysyła obraz. Emulowany
program to kilkanaście linii: bierze ten bajt i przesuwa duszka o dwa piksele w każdą
wciśniętą stronę — to samo robi gra z kartridża, tylko ma więcej kodu.

## Dlaczego mapa jest obrócona o ćwierć obrotu

Obraz konsoli jest szerszy niż wyższy, więc panel pracuje w poziomie i tak trzymasz
płytkę. Styki drążka są przylutowane na stałe i obracają się razem z nią. Płytka nie
pyta, jak ją trzymasz: drążek leży pod palcem tak samo, ale „góra" znaczy teraz co innego.

| kierunek, w który pchasz drążek | nóżka | krawędź płytki w trzymaniu pionowym |
|---|---|---|
| góra | PB0 | prawa |
| prawo | PB4 | dolna |
| dół | PB6 | lewa |
| lewo | PC0 | górna |

Ta tabela to trzymanie pionowe obrócone razem z płytką o ćwierć obrotu, dlatego w każdym
wierszu kierunek i krawędź nie pasują do siebie. Jeden ruch drążka w dłoni to jeden ruch
duszka na ekranie. W kodzie te pary siedzą w tablicy `pad_map`, a komentarz przy każdym
wpisie mówi, na której krawędzi płytki leży styk. Gdyby ktoś wpisał do `pad_map` nóżki
z trzymania pionowego, drążek działałby dalej, tylko obrócony: góra przesuwałaby duszka
w prawo, prawo w dół, dół w lewo, a lewo w górę.

## Nóżka, którą trzeba odebrać debugerowi

PB4 nie jest zwykłym wejściem: układ ma port debugowania, czyli wejście dla debugera,
programu z komputera, który podgląda płytkę. Po resecie pięć nóżek należy do tego portu.
PB4 jest wśród nich, pod nazwą NJTRST, którą posługuje się dokumentacja układu. Dopóki port
jest jej właścicielem, styk drążka nie daje się odczytać: nóżka odpowiada układowi
debugowania, a nie rejestrowi wejść, więc jeden kierunek wygląda, jakby był wciśnięty bez
przerwy.

`input_init` odbiera ją na starcie. Najpierw mówi blokowi konfiguracji systemu, że
debugowanie ma zostać przy SWD, czyli dwuprzewodowym porcie debugowania na nóżkach PA13
i PA14, a dopiero potem ustawia PB4 jako zwykłe wejście i włącza rezystor podciągający.
Kolejność ma znaczenie: dopóki port debugowania jest właścicielem nóżki, wpis w rejestrze
wejść nic nie daje.

## Co powinieneś zobaczyć

Zobaczysz ścianę z niebiesko-błękitnych kafli, zieloną podłogę i dwa prostokąty. Po ekranie
chodzi żółty duszek z czarnym narożnikiem, narysowany jako zwykły kwadrat, bo obraz nie jest
tu tematem. Drążek nim steruje, a wciśnięty niebieski przycisk zmienia go na czerwony
i zapala pierwszy kwadrat w dolnym rzędzie.

Ten rząd ośmiu kwadratów to bajt, który dostał program, wypisany bit po bicie, w kolejności
wychodzenia z rejestru. Niebieski przycisk trafia na bit A, więc zapala pierwszy kwadrat od
lewej. Wciśnij lewo i zobacz, że świeci siódmy od lewej, bo A, B, Select i Start wychodzą
z pada pierwsze. Płytka ma pięć styków, więc naraz świeci co najwyżej pięć kwadratów:
pierwszy od lewej i cztery ostatnie. Bity B, Select i Start zostają zerami, bo nie ma ich
czym wcisnąć.

Duszek idzie dwa piksele na klatkę. Samo wysłanie obrazu zajmuje te same 24,6 milisekundy,
o których czytałeś przy pasmach, więc klatka nie wypada częściej niż około czterdzieści razy
na sekundę, a do tego dochodzi jeszcze czas rysowania. Program wysyła obraz jednym kawałkiem
i czeka, aż panel go przyjmie; przerwań z etapu 10 tu nie ma, bo w czasie transmisji nie ma
nic innego do roboty.

## Zbuduj i sprawdź

Z katalogu `tutorial-pl` uruchom `make STAGE=11 check`. Ta komenda tylko kompiluje kod
i nic nie wgrywa, a kompilacja przechodzi bez ostrzeżeń przy `-Wall -Wextra`; to cała
weryfikacja tego etapu. Na płytkę wgrywasz ten sam kod komendą `make STAGE=11 flash`;
obrazu nie zobaczysz na komputerze.

## Ćwiczenia

Wpisz do `pad_map` nóżki z trzymania pionowego (PC0 jako górę, PB4 jako dół, PB6 jako
lewo, PB0 jako prawo) i sprawdź, czy kierunki obracają się o ćwierć obrotu w prawo. Potem
wróć do poprawnych wartości.

Zostaw latch podniesiony na czas odczytu, wołając w `read_pad` samo `pad_write_latch(1)`.
Wszystkie odczyty zwrócą wtedy ten sam bit A, czytany z nóżki na bieżąco: drążek przestanie
działać, a niebieski przycisk zapali cały rząd.

Odwróć kolejność bitów, wkładając je na miejsce `7 - i`. Niebieski przycisk przesunie
wtedy duszka w prawo, a pchnięcie drążka w prawo zmieni go na czerwony: kolejność bitów
nie jest umowna, siedzi w połączeniach pada.

Dodaj w `emulated_frame` warunek, który nie pozwala wejść na prostokąt: jeśli nowa pozycja
nachodzi na jeden z nich, cofnij ruch. To pierwszy kawałek prawdziwej logiki gry.

Zmień `HERO_STEP` z 2 na 8 i policz, ile klatek potrzeba na przejście ekranu. Pomyśl,
co by było, gdyby krok rósł razem z liczbą klatek na sekundę.

## Co dalej

Program reaguje na ciebie, ale obrazem rusza tylko on. W etapie 12 zobaczysz, jak gra
przewija obraz, nie przerysowując ani piksela, i jak jeden ekran pokazuje dwie różne
rzeczy naraz: stojący pasek statusu i ruchomą planszę pod nim.
