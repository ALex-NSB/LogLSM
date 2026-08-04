/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.проверка
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "rtc.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdio.h"
#include "string.h"
#include "usart.h"
#include "i2c.h"
#include "spi.h"
#include "quadspi.h"
#include "adc.h"
#include "dma.h"
#include "p25q128.h"
#include "Data.h"
#include "com.h"
#include "tmp117.h"
#include "fm25xx.h"
#include "lsm6dso_bus.h"
#include "stm_temp.h"
#include "iflash.h"      /* служебное хранилище внутренней Flash (журнал событий) */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
LSM6DSO_Object_t lsm;
P25Qx_HandleTypeDef flash;
uint8_t flash_powered;
RegistratorData regist;
uint8_t ble_flag;

/* --- Standby-архитектура, Стадия 1 (06.07.2026): причина пробуждения/сброса.
 * Выход из Standby = ПОЛНЫЙ reset → на загрузке узнаём, ЧТО разбудило, по флагам
 * (они переживают сброс). Определяется один раз в начале main()
 * (StandbyDetectWakeCause), дальше логика ветвится по g_wakeCause. */
#define WAKE_STANDBY   0x01u  /* вышли из Standby (был флаг SB), а не холодный старт */
#define WAKE_WKUP1     0x02u  /* WKUP1/PA0 — кабель/внешнее питание */
#define WAKE_WKUP2     0x04u  /* WKUP2/PC13 — INT1 датчика (движение) */
#define WAKE_RTC       0x08u  /* RTC wakeup-таймер (периодический) */
#define WAKE_POWERLOSS 0x10u  /* backup-домен был стёрт = питание пропадало (тумблер/
                               * вибрация зажима батареи в поле) */
volatile uint8_t g_wakeCause = 0;

/* «Часы обнулились на этом boot» — ставит MX_RTC_Init() (rtc.c) в ветке
 * отсутствия маркёра 0xBEBE, потребляет ServiceStorageBootLog() ниже (пишет
 * EVT_CLOCKZERO в журнал iflash). RAM-флаг живёт в пределах одного boot, чего
 * достаточно: обе точки — на раннем старте, до основного цикла. (02.08.2026) */
volatile uint8_t g_clockZeroed = 0;

/* Маркер «backup-домен жив» в RTC_BKP_DR0. Backup гаснет ТОЛЬКО с питанием
 * (ST-Link reset его не трогает). Маркер на месте на загрузке → это перешивка/
 * reset (НЕ питание); маркер пропал → питание пропадало (тумблер/провал, при
 * этом же теряется RTC → 1996). Надёжнее RCC-флага BORRST. */
#define BKP_ALIVE_MARKER 0xBEBE0001u

/* Счётчики рестартов (Стадия 1b, 06.07.2026) — хранятся в backup-регистрах RTC
 * (переживают reset/Stop2/Standby, гаснут только с backup-доменом = потеря
 * питания). Отдаются в GET_STATS → поля «Перезапуски по таймеру/питанию» на
 * дашборде (раньше были жёсткие нули). Требование: устройство должно корректно
 * восстанавливаться после потери питания (батарейка в зажиме + вибрация оси
 * колеса → кратковременные разрывы). Оживлены здесь: g_restartsTimer =
 * watchdog-сбросы (зависон, сторож вытащил); g_restartsPower = сбросы по
 * питанию/BOR. */
uint16_t g_restartsTimer = 0;
uint16_t g_restartsPower = 0;

/* IWDG — сторож ЖИВОГО кода (17.07.2026, wake_architecture §3). Включается
 * только если опционный бит IWDG_STOP стоит на «заморозку в Stop» (иначе
 * сбрасывал бы прибор в легитимном долгом сне). g_iwdg_on=1 после включения.
 * iwdg_kick() — рефреш; зовём в главном цикле, в Service() и в долгих
 * операциях (стирание). Объявлен в globals.h для доступа из com.c. */
volatile uint8_t g_iwdg_on = 0;
void iwdg_kick(void) { if (g_iwdg_on) IWDG->KR = 0x0000AAAAu; }
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void lsm6dso_init(void);
static void ServiceClock_Config(void);
static void WorkClock_Config(void);   /* штатный клок Работы: HCLK 16 МГц (26.07.2026) */
static void StandbyDetectWakeCause(void);   /* Стадия 1 Standby: причина пробуждения */
static void ServiceStorageBootLog(void);    /* Стадия 1b: причина сброса → журнал iflash */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
uint8_t wkup1_pin_set(void)
{
  return (GPIO_PIN_SET == HAL_GPIO_ReadPin(WKUP1_GPIO_Port, WKUP1_Pin));
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  WorkClock_Config();   /* HCLK = 16 МГц ДО инициализации периферии (26.07.2026: разгон 80 только в Сервисе) */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */

  /* Стадия 1 Standby (06.07.2026): причина пробуждения по флагам PWR (SB/WUF1/
   * WUF2) и RTC (WUTF) — САМЫМ ранним, пока настройка WKUP/периферии ниже их не
   * тронула. Фундамент: выход из Standby = reset, здесь узнаём, что разбудило.
   * Пока Standby не реализован (спим в Stop2), SB не взводится — g_wakeCause
   * просто отражает WUF/RTC; поведение НЕ меняется (задел на Стадии 2–4). */
  StandbyDetectWakeCause();
  ServiceStorageBootLog();   /* Стадия 1b: залогировать причину сброса в журнал iflash */

  MX_DMA_Init();
  MX_USART2_UART_Init();

  /* PA0 (WKUP1) — 02.07.2026, найденный корень «ничего не работает» после
   * перехода Service()-входа на чистый уровень (wkup1_pin_set(),
   * HAL_GPIO_ReadPin). CubeMX для SYS_WKUP1 НЕ генерирует HAL_GPIO_Init
   * вообще (см. .ioc: PA0.Mode=SYS_WakeUp0 — просто отметка для PWR, не
   * обычный GPIO_Input с меткой) — пин оставался в дефолтном сбросовом
   * состоянии (Analog, MODER=11). У STM32L4 в Analog-режиме цифровой
   * входной триггер (Шмитта) аппаратно отключён ради нуля потребления —
   * HAL_GPIO_ReadPin() в этом состоянии не отражает реальный уровень на
   * ножке, читает недостоверно (по факту почти всегда 0). Это и было
   * замаскировано раньше безусловным входом в Service() (`if (1 || ...)`,
   * см. CLAUDE.md, диагностика 22.06.2026) — сам факт ненадёжного чтения
   * PA0 был уже известен, тогда обошли, не долечили.
   * Само пробуждение из Stop2 по WKUP1 (PWR_FLAG_WUF1) на это не
   * завязано — у PWR отдельный, не через GPIO-триггер, тракт детекта на
   * этот вывод, поэтому оно работало само по себе.
   * Фикс: явно перевести PA0 в цифровой Input — тогда HAL_GPIO_ReadPin()
   * (wkup1_pin_set()) начинает возвращать реальный уровень. Pull=NOPULL —
   * подтяжка на время Stop2 всё равно держится отдельно, через PWR
   * (HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, PWR_GPIO_BIT_0) ниже) —
   * обычный GPIO-pull в Stop2 не действует, поэтому здесь он не нужен.
   * Сделано здесь, не в gpio.c: Core/ — генерируется CubeMX и правится
   * только внутри USER CODE секций внутри функции; «USER CODE BEGIN 2» в
   * gpio.c расположен ПОСЛЕ закрывающей '}' MX_GPIO_Init() (файловая
   * область видимости, не внутри функции) — там нельзя писать
   * исполняемые операторы, только объявления функций/данных. */
  {
    GPIO_InitTypeDef wkup1Init = {0};
    wkup1Init.Pin  = GPIO_PIN_0;
    wkup1Init.Mode = GPIO_MODE_INPUT;
    wkup1Init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &wkup1Init);
  }

  /* Настройка пробуждения WKUP1 (PA0) — детект внешнего питания */
  HAL_PWREx_EnablePullUpPullDownConfig();
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, PWR_GPIO_BIT_0);
  HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_HIGH);

  /* Настройка пробуждения WKUP2 (PC13) — INT1 датчика (LSM6DSO), движение.
   * 02.07.2026: реализация PASSIVE-сна начата — раньше устройство никогда
   * не уходило в Stop2 вообще (main.c крутил while(1) без сна), WKUP2/PC13
   * был просто GPIO_Input без какой-либо функции пробуждения. Полярность
   * HIGH — INT1 у LSM6DSO по умолчанию push-pull, активный уровень HIGH
   * (H_LACTIVE=0 в CTRL3_C, не менялся). Сам детектор Wake-Up на датчике
   * включается отдельно в lsm6dso_init() (см. ниже) —
   * HAL_PWR_EnableWakeUpPin() только разрешает пину будить ядро из Stop2,
   * сам источник события (что именно вызовет фронт на INT1) настраивается
   * в самом чипе LSM6DSO. */
  /* PC13 (INT1/WKUP2) — тот же фикс, что для PA0 выше (добавлен следом,
   * 02.07.2026): HAL_GPIO_Init для него так же молча выпал из gpio.c при
   * регенерации .ioc. Код сейчас уровень PC13 через HAL_GPIO_ReadPin не
   * читает (только флаг WUF2 — у PWR свой тракт детекта, GPIO-триггер не
   * нужен), так что функционально это, вероятно, ничего не меняло — но
   * cmdSetAcquisitionData() (com.c, легаси DRDY-прерывание) ожидает пин в
   * определённом состоянии, и дёшево снять неизвестность целиком. */
  {
    GPIO_InitTypeDef wkup2Init = {0};
    wkup2Init.Pin  = GPIO_PIN_13;
    wkup2Init.Mode = GPIO_MODE_IT_RISING;  /* 05.07: EXTI13 — БУДИЛЬНИК из Stop2 */
    wkup2Init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &wkup2Init);
    /* КРИТИЧНО (05.07.2026): Stop2 выходит ТОЛЬКО по EXTI; SYS_WKUP2 будит
     * лишь Standby/Shutdown. Без EXTI13 фронт INT1 латчит флаг WUF2, но ядро
     * из Stop2 не поднимает — «Работа» не просыпалась НИ НА КАКОЙ скорости
     * (см. session_notes_2026-07-05_rabota_stop2_exti.md). EXTI13 поднимает
     * ядро; источник пробуждения ниже по-прежнему опознаётся флагом WUF2
     * (он латчится независимо, т.к. EWUP2 включён). SYS_WKUP2 в .ioc оставлен
     * как задел под будущий Standby-ярус (там EXTI не работает, WKUP — верно). */
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
    /* НЕ включаем EXTI15_10 глобально! Иначе INT1 (движение/Wake-Up) сыплет
     * прерывания в бодрый путь ПОСТОЯННО и мешает UART/Flash/поллу — после
     * включения из boot переставали отвечать и Тест, и Flash, хотя МК не спит
     * (05.07.2026). EXTI нужен ИСКЛЮЧИТЕЛЬНО как будильник Stop2 — включаем
     * его только на время сна в SLEEP-ветке ниже, вне сна держим выключенным. */
  }
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_C, PWR_GPIO_BIT_13);
  HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN2_HIGH);

  lsm6dso_init();  /* TODO: если вернётся Error_Handler — вынести в Service() */

  regist.lsm = &lsm;
  regist.state = REG_STATE_SLEEP;
  regist.noDetectCount = 0;
  regist.confirmPolls = 0;
  /* Автомат SLEEP->CONFIRM->ROTATING (режим A) не запускается сам по
   * себе при старте — только по команде CMD_START_REGISTER (0x1D,
   * cmdStartRegister() в com.c). 02.07.2026, по прямому указанию
   * пользователя. */
  regist.monitoringActive = 0;
  regist.testNoSleep = 0;   /* «Работа» по умолчанию; «Тест» (0x23) взводит */

  /* ── IWDG + RTC в ПАРЕ (modes_and_timers §3, 17.07.2026) ─────────────────
   * IWDG тикает и в Stop2 (LSI ~32 кГц, макс ~32 c), НЕ замораживаем. Защита от
   * ложного сброса в долгом сне — RTC-wakeup-таймер будит МК < 32 c и гладит
   * сторож (iwdg_kick при пробуждении). Так один механизм ловит И зависание
   * бодрого кода, И «мёртвый сон» (если пробуждение/RTC не дошло до рефреша —
   * IWDG сбросит → recovery). RTC-часы при этом идут всегда. Опционный байт не
   * нужен. /256 × 4095 ≈ 32 c; RTC-wake в глубоком сне ~25 c (см. ниже).
   * ⚠ IWDG после старта не выключается до сброса. */
  /* ⚠ Порядок как в HAL_IWDG_Init (18.07.2026, фикс зависания 07:18): СНАЧАЛА
   * старт (KR=CCCC — он же аппаратно включает LSI), потом конфиг PR/RLR. В
   * прежнем порядке (старт последним) LSI был выключен → SR-флаги синхронизации
   * не сбрасывались → вечный while ДО главного цикла (устройство молчало). */
  IWDG->KR  = 0x0000CCCCu;   /* СТАРТ сторожа (включает LSI) */
  IWDG->KR  = 0x00005555u;   /* доступ к PR/RLR */
  IWDG->PR  = 6u;            /* делитель /256 */
  IWDG->RLR = 4095u;         /* период ~32 c */
  while (IWDG->SR != 0u) { } /* дождаться применения PR/RLR (LSI уже тикает) */
  IWDG->KR  = 0x0000AAAAu;   /* рефреш (загрузка RLR) */
  g_iwdg_on = 1;
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    iwdg_kick();   /* рефреш сторожа в бодром цикле (17.07.2026) */
    /* Вход в SERVICE (режим B — полноценный захватывающий Service(), для
     * произвольных LTP-команд: «Тест памяти», «Мониторинг» и т.п.) —
     * 02.07.2026, финальная версия: ТОЛЬКО по текущему УРОВНЮ WKUP1
     * (потенциал на пине), не по фронту/флагу WUF1. Понятие «фронта» тут
     * вообще не нужно — определить, «на стенде мы или нет» (кабель
     * подключён или нет), можно в любой момент простым чтением уровня,
     * WUF1 (защёлкнутый флаг события пробуждения из Stop2) для этой
     * задачи не подходит и не используется: он не взводится, если
     * устройство и так не спало (например, уже было в Service() на
     * предыдущей итерации). Пока WKUP1=1 — устройство однозначно в
     * Service(), без исключений.
     *
     * ⚠ Гейт !monitoringActive добавлен 03.07.2026 (первая же проверка
     * на стенде показала): без него запущенный командой автомат (режим A)
     * при подключённом кабеле (а на стенде он подключён ВСЕГДА — через
     * Nucleo) немедленно затягивался обратно в Service() на первой же
     * итерации после QUIT — обещание «после запуска от WKUP1 не зависит»
     * не выполнялось, автомат не делал ни одного шага, CMD_CYCLE_PUSH
     * не приходил («вкладка Стенд перестала работать»). Пока автомат
     * активен — в Service() не входим; выход из режима A: основной —
     * CMD_STOP_REGISTER (0x22, доходит в бодрых фазах через ComPoll
     * ниже), дополнительный — фронт WKUP1 в Stop2 (см. WUF1-ветку). */
    if (!regist.monitoringActive && wkup1_pin_set())
    {
      /* Перед SERVICE: полная скорость HSI (16 МГц вместо 4 МГц).
       * ComInit() внутри Service() переинитит UART под новую тактовую.
       * lsm6dso_init() вызывается повторно — I2C1 получит правильные
       * делители для 16 МГц (при первом вызове выше он стоит на 4 МГц). */
      ServiceClock_Config();
      lsm6dso_init();
      /* Service() (com.c) распознаванием вращения не занимается вообще
       * (убрано 02.07.2026) — только LTP-команды. Гироскоп поэтому здесь
       * не включаем: он нужен только автомату SLEEP->CONFIRM->ROTATING
       * ниже, который Service() не трогает. */
      Service(&regist);
      /* WUF1 для входа в Service() больше не используется (см. выше) —
       * сбрасываем его на всякий случай перед следующим Stop2, чтобы не
       * оставался взведённым от предыдущего пробуждения. */
      __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF1);
      /* ⚠ 28.07.2026 ФИКС «Работа не просыпалась». Договорённость 26.07: 80 МГц
       * ТОЛЬКО в Сервисе, «Работа» на минимале 16 МГц. Сервис поднимал клок до
       * 80 (PLL) и переинициализировал UART (ComInit) под 80. На ВЫХОДЕ из
       * Сервиса надо вернуть И клок (16 МГц, HSI), И UART/DMA под 16 — иначе:
       *  (1) автомат входил в Stop2 на 80 МГц, откуда accel-INT/EXTI13 не
       *      поднимал ядро → «Работа» висла в первом сне (все «нет ответа»);
       *  (2) даже проснувшись, CYCLE_PUSH шёл на неверном бод (UART от 80).
       * На рабочей 20.07 весь режим был 16 МГц (Сервис тоже) — потому работало.
       * Дифф main.c 20.07↔тек: клок — единственное отличие в пути сна. */
      WorkClock_Config();   /* назад на 16 МГц (HSI, PLL off) */
      ComInit();            /* UART/DMA заново под 16 МГц (BRR 921600) */
    }

    /* Автомат SLEEP->CONFIRM->ROTATING (режим A) — ЕДИНСТВЕННЫЙ
     * (Service() его не дублирует). 02.07.2026, финальная версия:
     * запускается ТОЛЬКО командой CMD_START_REGISTER (0x1D,
     * cmdStartRegister()/com.c → regist.monitoringActive=1), не по
     * состоянию WKUP1 — пока команда не пришла, монитор простаивает.
     * После запуска работает независимо от УРОВНЯ WKUP1 (верхняя
     * проверка гейтится !monitoringActive — 03.07.2026). Пути остановки:
     * основной — CMD_STOP_REGISTER (0x22) в бодрой фазе (ComPoll ниже),
     * дополнительный — ФРОНТ WKUP1 во время Stop2 (см. WUF1-ветку).
     *
     *   REG_STATE_SLEEP    — гироскоп выключен, включён только
     *                        акселерометр (Wake-Up Detection) — реальный
     *                        Stop2, без исключений (не «экономии ради» —
     *                        ради того, чтобы условия выхода из сна
     *                        проверялись одинаково что на столе, что в
     *                        поле).
     *   REG_STATE_CONFIRM  — проснулись по акселерометру (WKUP2),
     *                        гироскоп включён на CONFIRM_WINDOW_POLLS
     *                        опросов — подтверждаем реальное вращение
     *                        (не удар/тряску) порогом
     *                        RotationDetected()/GYRO_THRESHOLD.
     *   REG_STATE_ROTATING — вращение подтверждено, идёт цикл, дебаунс
     *                        ROTATION_DEBOUNCE_N перед завершением.
     *
     * CONFIRM и ROTATING — общая RotationStateStep() (com.c/com.h),
     * используется только отсюда (Service() её больше не зовёт).
     * PushCycleRecord() внутри неё сама проверяет ТЕКУЩИЙ уровень WKUP1
     * в момент завершения цикла: кабель есть — дублирует данные цикла по
     * LTP (активная фаза чуть удлиняется на время передачи), кабеля нет
     * — просто ничего не шлёт. В ОБОИХ случаях после этого — назад в
     * REG_STATE_SLEEP, без исключений. */
    if (regist.monitoringActive && regist.state != REG_STATE_SLEEP)
    {
      /* Бодрая фаза (CONFIRM/ROTATING) — МК не спит, попутно обслуживаем
       * LTP (03.07.2026): это ОСНОВНОЙ канал остановки теста со стенда
       * (CMD_STOP_REGISTER/0x22 — cmdStopRegister() сам закроет идущий
       * цикл и снимет monitoringActive; после этого верхняя проверка
       * заведёт в Service()). Только при кабеле: без WKUP1 слушать
       * некого, а DMA-приём мог вообще не инициализироваться (ComInit()
       * вызывается только в Service()). В SLEEP (Stop2) LTP недостижим
       * по определению — команды в паузах теста уйдут в таймаут, это
       * штатно (LOGLSMW не должен опрашивать 0x8D во время теста). */
      if (wkup1_pin_set())
        ComPoll();
      PushWdgKick();   /* kick-метка и из бодрых фаз (троттл 20 c внутри), 18.07 */
      RotationStateStep(&regist);
    }
    else /* REG_STATE_SLEEP (в т.ч. когда мониторинг ещё не запущен) */
    {
      if (regist.monitoringActive && regist.testNoSleep)
      {
        /* «Тест» (0x23): в фазе SLEEP НЕ уходим в Stop2 — контроллер всё
         * время на связи. Тот же фронт INT1/WKUP2, что будит из сна в
         * «Работе», здесь взводит флаг WUF2 на ходу (WUFx латчится и в
         * Run-режиме) — ловим его опросом, видно, что «прерывание пришло».
         * WUF1 (фронт кабеля/тумблера) → стоп в Service, как и в Stop2-
         * ветке. WUF2 → включаем гироскоп и уходим в CONFIRM. Начальный
         * сброс обоих флагов — в cmdStartTest() (чистый «взвод»). Никакого
         * Stop2/SuspendTick — просто крутимся и опрашиваем UART/флаги. */
        if (wkup1_pin_set())
          ComPoll();

        if (0 != __HAL_PWR_GET_FLAG(PWR_FLAG_WUF1))
        {
          __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF1);
          regist.monitoringActive = 0;   /* верхняя проверка → Service() */
        }
        else if (0 != __HAL_PWR_GET_FLAG(PWR_FLAG_WUF2))
        {
          __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF2);
          LSM6DSO_GYRO_Enable(&lsm);
          regist.confirmPolls = 0;
          regist.state = REG_STATE_CONFIRM;
        }
      }
      else
      {
      /* Реальный Stop2, независимо от текущего уровня WKUP1 и от того,
       * запущен ли мониторинг (см. комментарий выше). Если мониторинг
       * ещё НЕ запущен командой — уходим в Stop2 всё равно, чтобы не
       * крутить пустой цикл вхолостую в ожидании первой команды; WKUP2 в
       * этом случае просто игнорируется (см. ниже) — ждём либо подключения
       * кабеля (WKUP1), либо запуска мониторинга. Разбудит либо WKUP1/PA0
       * (на следующей итерации верхняя проверка прочитает уровень и, если
       * он «1», уйдёт в Service() — без разбора, был ли это фронт),
       * либо WKUP2/PC13 (INT1 датчика — переходим в CONFIRM, если
       * мониторинг запущен).
       *
       * HAL_SuspendTick()/HAL_ResumeTick() — без них SysTick (1 мс)
       * будил бы ядро почти сразу же после входа в Stop2, не дожидаясь
       * WKUP1/WKUP2.
       *
       * ВАЖНО про клок после пробуждения: восстанавливаем
       * ServiceClock_Config() (16 МГц), а не SystemClock_Config()
       * (номинально «WORK», HSI/4 = 4 МГц) — потому что фактически
       * прошивка ВСЕГДА работает на 16 МГц (см. подробности в
       * ServiceClock_Config() ниже и в session_notes_2026-06-30_
       * registrator_fixed.md). lsm6dso_init() повторно НЕ нужен (клок
       * после сна — тот же самый, 16 МГц).
       *
       * Флаги WUF1/WUF2 и уровень WKUP1 — НЕ взаимоисключающие проверки,
       * это два разных механизма для двух разных задач, друг другу не
       * мешают:
       *   — ФРОНТ (флаг WUFx) — единственный физический способ разбудить
       *     ядро ИЗ Stop2: пока МК спит, код не выполняется, «прочитать
       *     уровень» некому — будит именно аппаратное событие на пине
       *     (см. HAL_PWR_EnableWakeUpPin() выше). Это касается ОБОИХ
       *     источников, WKUP1 и WKUP2 — фронт всегда остаётся вектором
       *     выхода из полного сна, для обоих пинов одинаково.
       *   — УРОВЕНЬ (wkup1_pin_set()) — способ, которым код (когда он
       *     уже выполняется, не важно, разбужен только что или нет)
       *     определяет ТЕКУЩЕЕ состояние «на стенде мы или нет». Именно
       *     этим определяется вход в Service() наверху цикла — и это
       *     годится в любой момент, не только сразу после пробуждения.
       * Поэтому ниже проверяем ОБА флага, WUF1 и WUF2, симметрично —
       * узнаём, что именно разбудило Stop2 сейчас (сам факт пробуждения
       * по WKUP1 ни на что не влияет здесь и дальше явно не
       * обрабатывается: верхняя проверка уровня на следующей итерации
       * сама решит, входить в Service() или нет — это НЕ то же самое,
       * что «WUF1 не нужен»: он взводится и чистится, просто не
       * используется как условие ветвления в этом месте).
       *
       * TODO: не проверено на столе — само вхождение в Stop2, ток
       * потребления, факт пробуждения по обоим источникам, стабильность
       * I2C/RTC после серии засыпаний. Поведение «спать, пока мониторинг
       * не запущен» — разумное умолчание, явно с пользователем не
       * обсуждалось, требует подтверждения. */
      /* 05.07.2026: ЧИСТИМ WUFx ПЕРЕД сном. На стенде кабель держит WKUP1
       * статически HIGH → EWUP1, включённый при высоком уровне, залипляет
       * WUF1. Старая ветка ниже видела этот залипший WUF1 после пробуждения
       * и ложно роняла monitoringActive=0 → автомат уходил в Service,
       * «Работа» не писала НИ ОДНОГО цикла (нет ответа на всех скоростях
       * даже после того, как EXTI13 стал реально будить из Stop2). */
      __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF1);
      __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF2);
      __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_13);
      HAL_NVIC_ClearPendingIRQ(EXTI15_10_IRQn);

      /* 3-УРОВНЕВЫЙ АВТОМАТ (11.07.2026): источник пробуждения зависит от фазы.
       *  - deepSleep=1 (ГЛУБОКИЙ СОН, долго нет активности): будит ТОЛЬКО
       *    движение — accel Wake-Up/INT1 через EXTI13, RTC ВЫКЛЮЧЕН. Никакого
       *    опроса в покое → автономность. Спин НЕ грозит: в покое мотор стоит,
       *    вибрации нет → INT1 не спамит (спин раньше давал арм EXTI13 во время
       *    активности — здесь EXTI13 армится ИСКЛЮЧИТЕЛЬНО в покое).
       *  - deepSleep=0 («ПРОВЕРКА», недавно была активность): периодический RTC
       *    (~2 c) для проверки длительности/скорости; EXTI13 НЕ армим (мотор
       *    может крутиться → сыпал бы INT1).
       * accel Wake-Up (417 HP + Enable_Wake_Up_Detection) настроен в
       * cmdStartRegister — INT1 маршрут готов для обоих режимов. */
      /* Глубокий сон в «Работе» — БЕЗУСЛОВНО (11.07.2026, по решению
       * пользователя): «Работа» = полноценный автономный режим, глубокий сон
       * даже на стенде с кабелем — чтобы проверять реальное полевое поведение.
       * Кабельная работа (Flash-scan, сервис, всегда на связи) — это режим
       * «Тест» (testNoSleep=1, не спит). Следствие: в «Работе» спящее
       * устройство недостижимо по LTP (бар Флеш покажет «сканирование…»/нет
       * ответа, 0x22 не дойдёт в глубоком сне) — это ожидаемо, для связи —
       * «Тест» или пробуждение движением. */
      uint8_t deep = regist.deepSleep;   /* deep=1 = ПАССИВ (дремота), deep=0 = «Проверка» */
      if (deep)
        HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);   /* Пассив: будит ДВИЖЕНИЕ (accel/EXTI13) */
      /* RTC-wakeup-таймер в ОБОИХ ярусах (design B, IWDG+RTC, 17.07.2026):
       * период < таймаут IWDG, на пробуждении гладим сторож (iwdg_kick в цикле)
       * и не «спим навсегда». «Проверка» — ~1 c; Пассив — страховочный ~25 c
       * (< 32 c IWDG), редко → автономность почти цела (+<1 мкА). Часы (LSE)
       * идут всегда независимо; здесь — именно периодическое ПРОБУЖДЕНИЕ. */
      HAL_NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
      HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
      HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, deep ? 25u : 1u, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);

      HAL_SuspendTick();
      HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
      HAL_ResumeTick();

      HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
      HAL_NVIC_DisableIRQ(RTC_WKUP_IRQn);
      HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);   /* проснулись — источники сна больше не нужны */
      WorkClock_Config();   /* Работа — штатные 16 МГц (26.07.2026) */
      PushWdgKick();   /* RTC-глажение сторожа в «Работе» → метка 0x25 при кабеле (18.07.2026) */

      /* Источник пробуждения из ПАССИВА (design B, 17.07.2026): WUF2 (фронт
       * INT1/PC13, EWUP2 включён) = ДВИЖЕНИЕ → в «Проверку». WUF2 чист = только
       * RTC-страховка IWDG → сторож поглажен в цикле, гироскоп НЕ будим, спим
       * дальше в Пассиве (автономность цела). «Проверка» (deep=0) — всегда проверка. */
      uint8_t movementWake = 1;
      if (deep)
      {
        movementWake = __HAL_PWR_GET_FLAG(PWR_FLAG_WUF2) ? 1u : 0u;
        if (movementWake) { regist.deepSleep = 0; regist.idleChecks = 0; }
        else                regist.deepSleep = 1;
      }

      /* Проснулись из Stop2. Источник — RTC (в «Проверке») или accel-INT/EXTI13
       * (из глубокого сна, по движению), см. deep-логику выше. ЛЮБОЙ выход
       * трактуем как «пора проверить вращение» → гироскоп + CONFIRM (там
       * отсеивается отсутствие вращения от настоящего). Флаги WUFx как признак
       * источника НЕ используем. Остановку кабелем во сне не поддерживаем
       * (нужен EXTI0); штатный стоп — CMD_STOP_REGISTER 0x22 в бодрой фазе
       * CONFIRM/ROTATING (ComPoll выше). */
      /* movementWake=0 (только RTC-страховка IWDG в Пассиве) → гироскоп НЕ
       * трогаем, верх цикла снова уложит в Пассив (17.07.2026). */
      if (movementWake && regist.monitoringActive)
      {
        /* ГИРОСКОП-ТРОТТЛ Пассивный→Активный (12.07.2026). Гироскоп — самый
         * дорогой узел, поэтому НЕ крутим его на каждом accel-пробуждении: accel
         * Wake-Up слушаем всегда (Пассивный, дёшево), а вход в Активный (гироскоп
         * + CONFIRM) разрешаем не чаще раза в gyroRecheckSec. Порог измеряем по
         * всегда-идущему календарю RTC (1 с). gyroCheckPending=1 (старт/после
         * цикла) форсирует первую проверку без троттла. Из тишины elapsed велик
         * → проверка сразу (задержка ≈ прогрев 0.4 c). На упорной вибрации без
         * вращения — дешёвый возврат в глубокий сон, интервал эскалирует (см.
         * com.c). */
        RTC_DateTime nowTs;
        RTC_GetTimeDate(&nowTs);
        uint32_t sinceGyro = RTC_SubTimeDateSec(&nowTs, &regist.lastGyroCheck);
        /* Троттлим ТОЛЬКО пробуждения из ГЛУБОКОГО сна (deep=1, источник = accel-
         * INT). RTC-опросы фазы «Проверка» (deep=0) НЕ троттлим — именно они
         * добирают маргинальный низкооборотный старт за пару опросов; если их
         * пропускать, реальный старт срезается (был −8 c на 7 об/мин). */
        if (deep && !regist.gyroCheckPending && sinceGyro < regist.gyroRecheckSec)
        {
          /* Рано для гироскопа — Активный НЕ трогаем. Дешёвое пробуждение назад
           * в Пассивный (глубокий сон, accel слушаем дальше). Ток минимальный. */
          regist.deepSleep = 1;
          /* state остаётся REG_STATE_SLEEP → верх цикла снова уложит в Stop2 с
           * armed accel, гироскоп не включался. */
        }
        else
        {
        regist.gyroCheckPending = 0;
        regist.lastGyroCheck    = nowTs;
        /* НЕТ МЁРТВОГО ВРЕМЕНИ (11.07.2026): старт цикла = МОМЕНТ ПРОБУЖДЕНИЯ
         * (в глубоком сне это фронт движения/accel-INT = реальный пуск мотора).
         * Фиксируем СРАЗУ, ДО прогрева/CONFIRM — счёт идёт с этого мгновения,
         * без задержки на подтверждение. Если вращение не подтвердится (CONFIRM
         * истечёт) — цикл просто не запишется (сброс). Убирает остаток −latency
         * от прогрева+опросов CONFIRM. */
        regist.rot.startTimeStamp = nowTs;
        regist.rot.lastRotStamp = regist.rot.startTimeStamp;
        regist.rot.maxRate = regist.rot.maxVibr = 0;
        regist.rateSum = 0; regist.rateCount = 0; regist.avgRate = 0;
        regist.rotWarmup = 1;
        LSM6DSO_GYRO_Enable(&lsm);
        /* ПРОГРЕВ ГИРОСКОПА (11.07.2026). После сна гироскоп был выключен;
         * сразу после Enable первые отсчёты — мусор/ноль (turn-on). А
         * Poll_Sensor ждёт DRDY АКСЕЛЕРОМЕТРА (сейчас 417 Гц → DRDY каждые
         * ~2.4 мс), поэтому все 5 опросов CONFIRM пролетают за ~35 мс — раньше,
         * чем гироскоп (12.5 Гц) выдаст валидный отсчёт → RotationDetected=ложь
         * даже при вращении (счётчик RTC тикал сквозь вращающийся стенд-цикл,
         * но цикл не писался). Ждём ~200 мс (2-3 периода гиро) — тогда CONFIRM
         * читает устоявшуюся угловую скорость и ловит вращение.
         * 200→400 мс (11.07.2026): 1-й цикл на 10 об/мин терялся — 10 об/мин у
         * самого порога (≈857 LSB против GYRO_THRESHOLD 600, запас ~1.4×), за
         * 200 мс свежий гироскоп не успевал выйти на стабильное значение и
         * проваливался ниже порога («Общее» −10 c). 400 мс = ~5 отсчётов при
         * 12.5 Гц — полное успокоение, нижний порог ловится надёжно. */
        HAL_Delay(400);
        regist.confirmPolls = 0;
        regist.state = REG_STATE_CONFIRM;
        } /* else — троттл разрешил гироскоп-проверку (вход в Активный) */
      }
      } /* else — реальный Stop2 (не «Тест») */
    }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV4;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* Standby-архитектура, Стадия 1 (06.07.2026): определить причину пробуждения/
 * сброса ДО того, как логика начнёт трогать флаги. Выход из Standby = reset →
 * узнаём, что разбудило, по защёлкнутым флагам (PWR SB/WUF1/WUF2 и RTC WUTF
 * переживают сброс). Результат — в g_wakeCause.
 * Чистим ТОЛЬКО SB (иначе останется взведён навсегда и следующий холодный старт
 * ошибочно прочитается как «выход из Standby»). WUF1/WUF2/WUTF НЕ чистим здесь —
 * они ещё нужны логике ветвления ниже; их гасит уже она. */
static void StandbyDetectWakeCause(void)
{
  g_wakeCause = 0;
  if (__HAL_PWR_GET_FLAG(PWR_FLAG_SB))
  {
    g_wakeCause |= WAKE_STANDBY;
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_SB);
  }
  if (__HAL_PWR_GET_FLAG(PWR_FLAG_WUF1)) g_wakeCause |= WAKE_WKUP1;
  if (__HAL_PWR_GET_FLAG(PWR_FLAG_WUF2)) g_wakeCause |= WAKE_WKUP2;
  if (__HAL_RTC_WAKEUPTIMER_GET_FLAG(&hrtc, RTC_FLAG_WUTF)) g_wakeCause |= WAKE_RTC;
}

/* Стадия 1b (06.07.2026): причина СБРОСА по RCC-флагам → событие в журнал
 * внутренней Flash (iflash, переживает потерю питания). Даёт реальные счётчики
 * «Перезапуски по таймеру/питанию» в GET_STATS (были жёсткие нули).
 * totalSec пока 0 — внешняя Flash на этом раннем этапе ещё не инициализирована
 * (уточнить позже, залогировав после LoadTotalSec). Флаги обязательно чистим —
 * иначе накопятся и следующий сброс прочитается неверно. */
static void ServiceStorageBootLog(void)
{
  /* Гарантировать, что журнал пригоден к записи: если страницы 124–127 никогда
   * не инициализировались (мусор, не 0xFF) — стереть один раз. Иначе append не
   * находил свободный слот и событие не писалось (счётчик оставался 0). */
  iflash_journal_ensure_ready();

  uint8_t rcc = 0;
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) rcc |= 0x01u;   /* watchdog (был зависон) */
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))  rcc |= 0x04u;   /* POR/BOR/PDR (в rccMask) */
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))  rcc |= 0x08u;   /* программный reset (ST-Link Run) */
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))  rcc |= 0x10u;   /* NRST-пин (в rccMask) */

  /* Маркер-подход отброшен (07.07.2026): backup-домен на плате переживает щелчок
   * тумблером (VBAT/конденсатор), а «часы в 1996» — это MX_RTC_Init
   * переинициализирует время на КАЖДОМ boot (Check_RTC_BKUP пуст), а НЕ признак
   * потери питания. Детектим проще и надёжно: watchdog = IWDGRST; всё, что НЕ
   * программный reset (SFTRST — это ST-Link «Run/Debug»), считаем питанием
   * (POR/BOR/NRST = тумблер/провал контакта в поле).
   * ⚠ Если ST-Link настроен на АППАРАТНЫЙ reset (NRST без SFTRST) — перешивка
   * тоже посчитается; тогда переключить в IDE reset mode на «software system
   * reset» ЛИБО пересмотреть признак (по rccMask видно, что реально стоит). */
  /* СЧЁТЧИК рестартов (стр.124..127) работает ВСЕГДА — инкрементируется на каждом
   * реальном сбросе (в т.ч. «между жизнями» и при тестировании), сбрасывается
   * «Сброс WDT». В ЗАПИСЬ журнала активаций (стр.121) число попадает только при
   * событии активации/сохранения — а оно и так происходит лишь когда прибор
   * активирован. Поэтому гейт на сам счётчик НЕ нужен (03.08.2026, по уточнению:
   * «счётчик срабатывает/инкрементируется/сбрасывается, но не отражается в записи
   * пока не активен» — за «запись» отвечает стр.121, а не гейт здесь). */
  if (rcc & 0x01u)
  {
    iflash_journal_append(EVT_WATCHDOG, 0u, rcc);
  }
  else if (!(rcc & 0x08u))
  {
    iflash_journal_append(EVT_POWERLOSS, 0u, rcc);
    g_wakeCause |= WAKE_POWERLOSS;
  }
  /* SFTRST (перешивка/программный reset) и Standby-выход (флаг SB, PWR) — не сбои,
   * не логируем. Во время отладки (перешивки/щелчки питанием) события всё равно
   * попадут — поэтому провижн перед полем СТИРАЕТ журнал (см. design §6). */

  /* Обнуление часов (02.08.2026) — ОТДЕЛЬНАЯ ось от reset-причины выше. Флаг
   * взведён в MX_RTC_Init(), когда маркёр 0xBEBE в RTC_BKP_DR0 отсутствовал =
   * RTC реально уехал в 00:00 01.01.2000. Пишем EVT_CLOCKZERO → его накопленный
   * счётчик (iflash_journal_count) = «поколение часов», которое SaveParamOnEEPROM
   * кладёт в запись цикла [40..41]. Смена этого номера при чтении = точка стыка
   * журнала (время рестартовало). Именно marker-absence, а НЕ EVT_POWERLOSS:
   * последний считается по RCC-флагам и растёт даже когда часы НЕ обнулялись
   * (NRST/BOR при живом backup-домене). См. clock_epoch_seam_spec_v1.md. */
  if (g_clockZeroed)
    iflash_journal_append(EVT_CLOCKZERO, 0u, rcc);

  __HAL_RCC_CLEAR_RESET_FLAGS();
}

/* SERVICE режим: питание внешнее, ток не экономим.
 * HSI без делителя AHB = 16 МГц. FLASH_LATENCY_0 валиден до 16 МГц
 * при VRANGE1. HAL_RCC_ClockConfig автоматически перенастраивает SysTick
 * через HAL_InitTick — HAL_Delay/HAL_GetTick остаются правильными.
 *
 * Зачем: при старте SystemClock_Config ставит AHBCLKDivider=/4 → HCLK=4 МГц
 * (нужно для WORK-режима — I2C/SPI при нём будут реинициализироваться
 * с делителями на 4 МГц, а не на 16 МГц). В SERVICE-режиме это лишнее:
 * ComInit() вызывает MX_USART2_UART_Init() заново, поэтому BRR
 * пересчитается под 16 МГц → 921600 бод с погрешностью 0.08%
 * (вместо 0.64% при 4 МГц — принципиально улучшает надёжность UART). */
static void ServiceClock_Config(void)
{
  /* ⚠ КРИТИЧНО для выхода из Stop2 (найдено 09.07.2026, разбор кода).
   * На STM32L4 при STOPWUCK=0 (сброс. дефолт, здесь не менялся) выход из
   * Stop2 тактируется MSI, а HSI16 аппаратно ВЫКЛЮЧЕН. HAL_RCC_ClockConfig()
   * с источником HSI сам HSI НЕ включает — он лишь проверяет HSIRDY и при
   * выключенном HSI возвращает HAL_ERROR (см. stm32l4xx_hal_rcc.c: «HSI is
   * selected as System Clock Source» → if(HSIRDY==0) return HAL_ERROR).
   * Итог: после пробуждения из Stop2 ядро попадало сюда → Error_Handler()
   * (__disable_irq; while(1)) → вечный висяк с выключенными прерываниями.
   * Снаружи неотличимо от «не проснулось»: PING/0x23 таймаутят, UART мёртв —
   * ровно симптом «Работа» из session_notes_2026-07-05. «Тест» (0x23) этого
   * не касается: он в Stop2 не уходит, HSI не гаснет, эта функция после сна
   * не вызывается. Boot и вход в Service тоже безопасны — там HSI уже включён
   * SystemClock_Config()/предыдущим кодом.
   * ФИКС: включаем HSI ЯВНО (OscConfig) до переключения SYSCLK на него.
   * Пока работаем на MSI (после Stop2) включение HSI допустимо. */
  RCC_OscInitTypeDef osc = {0};
  osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  osc.HSIState            = RCC_HSI_ON;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  /* Разгон Сервиса до 80 МГц (26.07.2026): при входе в Service поднимаем
   * PLL, весь сервисный режим работает на 80 МГц — флеш через QSPI можно
   * гнать до 80 (реальная скорость чтения — параметр для потребителя).
   * HSI(16)/M2 ×N20 /R2 = 80 МГц. Питание уже Scale1 (см. SystemClock_Config).
   * ComInit() ниже переинитит UART под новую SystemCoreClock, связь 921600
   * сохранится. Выход в «Работу» возвращает 16 МГц (WorkClock_Config). */
  osc.PLL.PLLState        = RCC_PLL_ON;
  osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
  osc.PLL.PLLM            = 2;
  osc.PLL.PLLN            = 20;
  osc.PLL.PLLR            = RCC_PLLR_DIV2;
  osc.PLL.PLLP            = RCC_PLLP_DIV7;
  osc.PLL.PLLQ            = RCC_PLLQ_DIV2;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    Error_Handler();

  RCC_ClkInitTypeDef clk = {0};
  clk.ClockType      = RCC_CLOCKTYPE_HCLK  | RCC_CLOCKTYPE_SYSCLK
                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;   /* 80 МГц */
  clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  /* 80 МГц требует FLASH_LATENCY_4 (STM32L4 RM: 4 WS при Scale1 >64 МГц). */
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK)
    Error_Handler();
}

/* Штатный клок «Работы» — HCLK 16 МГц на HSI, PLL выключен (26.07.2026:
 * выделено из прежнего ServiceClock_Config, который теперь разгоняет до 80).
 * Возврат сюда при выходе Сервиса в «Работу» — рабочие энергорежимы и сон
 * рассчитаны на 16 МГц/HSI, их не трогаем. */
static void WorkClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  osc.HSIState            = RCC_HSI_ON;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  osc.PLL.PLLState        = RCC_PLL_NONE;   /* PLL не трогаем/выключаем */
  if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    Error_Handler();

  RCC_ClkInitTypeDef clk = {0};
  clk.ClockType      = RCC_CLOCKTYPE_HCLK  | RCC_CLOCKTYPE_SYSCLK
                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
  clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;   /* HCLK = 16 МГц */
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK)
    Error_Handler();
}

static void lsm6dso_init(void)
{
  LSM6DSO_IO_t pIO;
  pIO.Init     = I2C_LSMDSO_Init;
  pIO.DeInit   = I2C_LSMDSO_DeInit;
  pIO.BusType  = LSM6DSO_I2C_BUS;
  pIO.Address  = I2C_LSMDSO_ADDRESS;
  pIO.WriteReg = I2C_LSMDSO_WriteReg;
  pIO.ReadReg  = I2C_LSMDSO_ReadReg;
  pIO.GetTick  = LSMDSO_GetTick;
  pIO.Delay    = HAL_Delay;
  LSM6DSO_RegisterBusIO(&lsm, &pIO);
  LSM6DSO_Init(&lsm);

  LSM6DSO_ACC_SetFullScale(&lsm, 2);
  LSM6DSO_ACC_SetOutputDataRate_With_Mode(&lsm, 12.5f, LSM6DSO_ACC_ULTRA_LOW_POWER_MODE);
  LSM6DSO_GYRO_SetFullScale(&lsm, 2000);
  LSM6DSO_GYRO_SetOutputDataRate_With_Mode(&lsm, 12.5f, LSM6DSO_GYRO_LOW_POWER_NORMAL_MODE);

  /* Акселерометр включён всегда — он же и есть источник WKUP2 в PASSIVE
   * (Wake-Up Detection ниже). Гироскоп сознательно НЕ включается здесь
   * (02.07.2026, по замечанию пользователя про раздельное потребление):
   * ODR гироскопа выше только закэширован в pObj->gyro_odr (12.5 Гц
   * LOW_POWER_NORMAL — см. SetOutputDataRate_With_Mode выше), реально на
   * датчик не подаётся, пока LSM6DSO_GYRO_Enable() не вызван явно — при
   * переходе SLEEP->CONFIRM в main()/while(1), после подтверждённого
   * пробуждения по акселерометру (WUF2). Service() (com.c) гироскоп не
   * трогает вообще — распознаванием вращения не занимается. */
  LSM6DSO_ACC_Enable(&lsm);

  /* Wake-Up детектор датчика на INT1 -> WKUP2/PC13 (см. настройку
   * HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN2_HIGH) выше по main()) —
   * 02.07.2026, часть реализации PASSIVE (Stop2). Библиотечная
   * LSM6DSO_ACC_Enable_Wake_Up_Detection() сама переставляет ODR на
   * 417 Гц (ST-дефолт для этой фичи, см. lsm6dso.c) — это избыточно для
   * низкого энергопотребления, поэтому сразу после возвращаем ODR обратно
   * на 12.5 Гц ULTRA_LOW_POWER (её же ставили выше по этой функции).
   * Порог/длительность (threshold=0x02, duration=0x00) — дефолты самой
   * библиотеки, НЕ откалиброваны под реальный монтаж на оси колеса —
   * подбирать эмпирически на стенде (LSM6DSO_ACC_Set_Wake_Up_Threshold/
   * _Set_Wake_Up_Duration, пока не вызываются). */
  LSM6DSO_ACC_Enable_Wake_Up_Detection(&lsm, LSM6DSO_INT1_PIN);
  /* ⚠ История (03.07.2026, подтверждено на железе): возврат ODR на
   * 12.5 Гц ULTRA_LOW_POWER после Enable_Wake_Up_Detection ЛОМАЛ wake-up
   * полностью — в ультра-низком энергорежиме embedded-функции датчика
   * (slope/wake-up) не работают, устройство никогда не просыпалось по
   * WKUP2 (пуши не приходили, «Регистратор» пуст). На ST-шном дефолте
   * функции (417 Гц HP) пробуждение и вся цепочка WKUP2→CONFIRM→
   * ROTATING→пуш подтверждены на стенде.
   * Подбор рабочей точки (03.07.2026, на стенде):
   *   417 Гц HP — пуши ИДУТ (подтверждено);
   *   26 Гц LP  — пуши НЕ идут (подтверждено): будит не плавное
   *               вращение, а ВЧ-вибрация шагового мотора — 26 Гц её
   *               отфильтровывает, slope-события нет;
   *   104 Гц LP — ТЕКУЩИЙ кандидат (середина сетки 26→52→104→208→417;
   *               ток LP@104 ≈ 17 мкА против ~170 мкА у HP@417 —
   *               приемлемо для LS14250 на годы). Если не разбудит —
   *               следующий шаг 208 LP; если разбудит — можно попробовать
   *               52 LP ради экономии. В поле (ось колеса) источник
   *               пробуждения тоже вибрация/удары, не гладкое вращение —
   *               стендовый подбор адекватен полевому сценарию.
   * LSM6DSO_ACC_SetOutputDataRate_With_Mode(&lsm, 417.0f,
   *                            LSM6DSO_ACC_HIGH_PERFORMANCE_MODE); */
  /* 05.07.2026: вернули на ПОДТВЕРЖДЁННЫЙ 417 Гц HP. Кандидат 104 Гц LP
   * (стоял здесь с 03.07) на стенде НЕ будил из Stop2 ни на какой скорости
   * (42→300 — «нет ответа» на всех циклах, «Регистратор» пуст): низкий
   * LP-ODR отфильтровывает ВЧ-вибрацию шагового мотора, slope-события нет,
   * фронт INT1→WKUP2 не приходит. 417 HP — единственная проверенная точка,
   * где вся цепочка WKUP2→CONFIRM→ROTATING→пуш работает. Оптимизацию тока
   * (208/104/52 LP) откладываем — сперва подтверждаем сам автономный путь.
   * 05.07 ВЕЧЕР — ОТКАЧЕНО: 417 HP СЛОМАЛ «Тест» (wake-up перестал давать
   * событие → WUF2 не взводится → CONFIRM не стартует → пушей нет на любой
   * скорости). Вернул 104 LP — рабочая точка детекта «Теста». Подбор ODR под
   * Stop2/«Работу» — отдельный заход (session_notes_2026-07-05). */
  LSM6DSO_ACC_SetOutputDataRate_With_Mode(&lsm, 104.0f,
                                          LSM6DSO_ACC_LOW_POWER_NORMAL_MODE);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
