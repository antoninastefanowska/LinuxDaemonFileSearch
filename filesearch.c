#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <signal.h>
#include <limits.h>
#include <sys/wait.h>

// int v - zmienna globalna określająca czy została włączona funkcja -v (szczegółowe komunikaty) - przyjmuje wartości 0 lub 1
// int sygnal - zmienna globalna przechowująca numer otrzymanego sygnału (0 w przypadku braku sygnału)
int v, sygnal;

/* funkcja obsługi sygnału - zapisuje wartość otrzymanego sygnału do zmiennej globalnej */
void odbierz_sygnal(int signum)
{
	sygnal = signum;
}

/* funkcja przesyłająca podany komunikat do danych systemowych po dodaniu do niego daty i godziny */
// char* zawartosc - tekst do zapisania w logach
void wyslij(char* zawartosc)
{
	time_t t = time(NULL);
	struct tm data = *localtime(&t);
	char output[PATH_MAX];

	snprintf(output, sizeof(output), "%d-%d-%d %d:%d:%d  %s", data.tm_year + 1900, data.tm_mon + 1, data.tm_mday, data.tm_hour, data.tm_min, data.tm_sec, zawartosc);

	openlog("projekt", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
	syslog(LOG_NOTICE, output, getuid());
	closelog();
}

/* funkcja rekurencyjna poszukująca plików z podanym wzorcem w nazwie */
// char* sciezka - sciezka do aktualnie przeszukiwanego katalogu
// char* wzorzec - poszukiwany wzorzec
void szukaj(char* sciezka, char* wzorzec)
{
	if (sygnal) return; // wyjście z funkcji w przypadku nadejścia sygnału

	DIR *katalog = NULL;
	struct dirent *wpis;
	char sciezka_nowa[PATH_MAX];

	katalog = opendir(sciezka);

	if (katalog)
	{
		while (wpis = readdir(katalog)) // przeszukiwanie danego katalogu wpis po wpisie
		{
			if (!strcmp(wpis->d_name, ".") || !strcmp(wpis->d_name, "..")) // pominięcie wpisów . i ..
				continue;

			snprintf(sciezka_nowa, sizeof(sciezka_nowa), "%s/%s", sciezka, wpis->d_name); // dodanie nazwy wpisu do scieżki

			if (access(sciezka_nowa, R_OK)) // pominięcie wpisów bez prawa odczytu
				continue;

			/* komunikat porównania wysyłany do syslogu */
			char output[PATH_MAX];
			if (v)
			{
				snprintf(output, sizeof(output), "porownanie: %s z %s", wzorzec, sciezka_nowa);
				//wyslij(output);
			}

			/* sprawdzenie czy podany wzorzec jest fragmentem nazwy wpisu - jeśli tak, wysyłany jest komunikat do syslogu */
			if (strstr(wpis->d_name, wzorzec))
			{
				snprintf(output, sizeof(output), "odnaleziono: %s - %s", wzorzec, sciezka_nowa);
				//wyslij(output);
			}

			/* sprawdzenie czy wpis jest katalogiem i czy ma prawa odczytu i wykonania - jeśli tak,
			następuje kolejne wywołanie rekurencji (następuje wejście do tego katalogu i przeszukanie go) */
			if (wpis->d_type == DT_DIR && !access(sciezka_nowa, R_OK | X_OK))
				szukaj(sciezka_nowa, wzorzec);

            		if (sygnal) break; // wyjście z pętli w przypadku nadejścia sygnału
		}
		closedir(katalog);
	}
}

/* przesłanie do logów informacji o nadejściu danego sygnału */
void sygnal_powiadomienie()
{
	if (v)
	{
		if (sygnal == SIGUSR1)
			wyslij("odebrano SIGUSR1");
		else if (sygnal == SIGUSR2)
			wyslij("odebrano SIGUSR2");
	}
}

/* podproces działający w nieskończonej pętli */
// char* wzorzec - poszukiwany przez proces wzorzec
void podproces(char* wzorzec)
{
	char output[128];
	if (v)
	{
		snprintf(output, sizeof(output), "rozpoczeto skanowanie: %s", wzorzec);
		wyslij(output);
	}
	szukaj("/", wzorzec);
	sygnal_powiadomienie(); // jeśli nadszedł sygnał, który przerwał działanie funkcji, wysłane zostanie powiadomienie
	if (v)
	{
		snprintf(output, sizeof(output), "zakonczono skanowanie: %s", wzorzec);
		wyslij(output);
	}
	exit(0);
}

/* proces nadzorczy działający w nieskończonej pętli */
// char* wzorce[] - tablica z wzorcami
// int n - liczba podprocesów
// int czas - czas spania po zakończeniu podprocesów przed utworzeniem kolejnych
void proces_glowny(char* wzorce[], int n, int czas)
{
	pid_t podprocesy[n];
	int i, status;
	sigset_t sygnaly;
	siginfo_t info;
	struct timespec t;

	sigemptyset(&sygnaly);
	t.tv_sec = czas;
	t.tv_nsec = 0;

	while (1)
	{
		if (v) wyslij("obudzenie");

		/* uruchomienie po jednym podprocesie dla każdego wzorca */
		for (i = 0; i < n; i++)
		{
			podprocesy[i] = fork();
			if (podprocesy[i] == -1)
				return;
			else if (podprocesy[i] == 0)
				podproces(wzorce[i]);
		}

		while (wait(&status) > 0 && !sygnal); // czekanie na zakończenie podprocesów lub na nadejście sygnału

		if (sygnal) // jeśli zmienna globalna zapisała SIGUSR1 lub SIGUSR2
		{
			sygnal_powiadomienie();
			for (i = 0; i < n; i++)
				kill(podprocesy[i], sygnal); // przekazanie podprocesom otrzymanego sygnału
		}
		do
		{
			if (sygnal != SIGUSR1) // wcześniejsze nadejście sygnału SIGUSR1 powoduje
			{
				sygnal = 0;
				if (v) wyslij("uspienie");
				sigtimedwait(&sygnaly, &info, &t); // czekanie na nadejście sygnału lub na upłynięcie podanego czasu
				sygnal_powiadomienie(); // jeśli w trakcie spania nadeszło powiadomienie, wysłany będzie komunikat
			}
		} while (sygnal == SIGUSR2); // nadejście sygnału SIGUSR2 w trakcie spania demona powoduje jego ponowne uśpienie
		sygnal = 0;
	}

}

int main(int argc, char *argv[])
{
	pid_t pid;
	int i, n = 0, czas = 120; // n - liczba wzorców
	v = sygnal = 0;

	/* tworzenie demona */
	pid = fork();
	if (pid == -1)
		return -1;
	else if (pid != 0)
		exit(EXIT_SUCCESS);
	if (setsid() == -1)
		return -1;
	if (chdir("/") == -1)
		return -1;
	for (i = 0; i < 3; i++)
		close(i);
	open("/dev/null", O_RDWR);
	dup(0);
	dup(0);

	/* przeanalizowanie parametrów podanych przy wywołaniu programu */
	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-t"))
		{
			if (i < argc - 1) czas = atoi(argv[i + 1]);
			i++;
			continue;
		}
		else if (!strcmp(argv[i], "-v"))
			v = 1;
		else n++;
	}
	int j = 0;
	char* wzorce[n];
	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-t"))
		{
			i++;
			continue;
		}
		else if (strcmp(argv[i], "-v"))
		{
			wzorce[j] = argv[i];
			j++;
		}
	}

	/* ustawienie przechwytywania sygnałów */
	struct sigaction akcja;
	akcja.sa_handler = odbierz_sygnal;
	akcja.sa_flags = 0;
	if (sigemptyset(&akcja.sa_mask))
		return -1;
    if (sigaction(SIGUSR1, &akcja, NULL))
		return -1;
	if (sigaction(SIGUSR2, &akcja, NULL))
		return -1;

	/* ustawienie blokowania wszystkich sygnałów poza SIGUSR1, SIGUSR2, SIGTERM, SIGCHLD, SIGSTOP i SIGKILL */
	sigset_t sygnaly;
	sigfillset(&sygnaly);
	sigdelset(&sygnaly, SIGUSR1);
	sigdelset(&sygnaly, SIGUSR2);
	sigdelset(&sygnaly, SIGTERM);
	sigdelset(&sygnaly, SIGCHLD);
	sigdelset(&sygnaly, SIGSTOP);
	sigdelset(&sygnaly, SIGKILL);
	sigprocmask(SIG_BLOCK, &sygnaly, NULL);

	proces_glowny(wzorce, n, czas);

	return 0;
}
