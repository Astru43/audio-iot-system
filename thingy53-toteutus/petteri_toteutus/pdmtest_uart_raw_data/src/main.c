#include <stdlib.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/audio/dmic.h>
#include <dk_buttons_and_leds.h>
#include <nrfx_pdm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>


LOG_MODULE_REGISTER(pdmtest, LOG_LEVEL_DBG);

/* Pins for microphone */
#define PDM_CLK     41  // P1.09 = 32 + 9
#define PDM_DIN     27  // P0.27

/* Audio sampling settings 
 * Note: as of now, there's no automated way for making sure AUDIO_SAMPLE_RATE 
 * will produce valid output due to how the actual sampling rate is calculated (PDM_CLK / ratio) 
 * so you must make sure the sampling frequency aligns with a valid clock frequency and ratio */
#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_SAMPLE_SIZE       (sizeof(int16_t))
#define AUDIO_BLOCK_SIZE        ((AUDIO_SAMPLE_RATE * AUDIO_SAMPLE_SIZE) * 2)

/* How much to amplify the recorded samples by */
#define PCM_AMP 100

/* Set clock frequency and ratio. 
 * These need to produce the desired sample rate = (PDM_CLK_FREQ / PDM_RATIO)
 *
 * Note: this is not very thoroughly tested,
 * but it appears PDM_CLK >= 1.1MHz is required by the VM3011 so possible valid options are:
 * 
 * *------------*----------*---------------*
 * |    FREQ    |   RATIO  |   SAMPLE FREQ |
 * |------------|----------|---------------|
 * | 1.231 MHz  |    64    |   19.234 kHz  |
 * | 1.280 MHz  |    64    |       20 kHz  |
 * | 1.333 MHz  |    64    |   20.828 kHz  |
 * | 1.231 MHz  |    80    |   15.388 kHz  |
 * | 1.280 MHz  |    80    |       16 kHz  |
 * | 1.333 MHz  |    80    |   16.662 kHz  |
 * *------------*----------*---------------*
 *
 * As of 2024-03-21, these must be given in the form 
 * NRF_PDM_FREQ_1231K, NRF_PDM_1280K and so on.
 *
 * Settings for 16kHz */
#define PDM_CLK_FREQ    NRF_PDM_FREQ_1280K;
#define PDM_RATIO       PDM_RATIO80

/* How many buffers (seconds) we want to record. 
 * N_BUFF * 2 = seconds
 *
 * nrfx_pdm_buffer_set() only allows buffer buffer sizes <= 32767 words 
 * at a 16k sampling frequency, the most we can do per buffer
 * is about 2 seconds so >1 are required for longer periods 
 * Out of memory occurs after around 12 seconds */
#define N_BUFF  6 

K_SEM_DEFINE(data_ready, 0, 1);


enum PDM_RATIO{
    PDM_RATIO64 = 0,
    PDM_RATIO80 = 1,
};

static uint8_t g_buffsel = 0;
static int16_t *g_buff[N_BUFF];

static bool g_pdm_stopped = 0;

/* Quick and (very) dirty solution to changing the PDM sampling ratio */
void set_pdm_ratio(enum PDM_RATIO ratio){
    /* 0x50026520 PDM_RATIO register address for SECURE application 
     * If bit 0 is set, ratio will be 80.
     * If bit 0 is unset, ratio will be 64.*/
    __asm__ volatile("ldr r1, =0x50026520\n\t"
                     "str %0, [r1]\n\t"
                      :
                      : "r" (ratio));
}


//Configure for UART
#define UART_PRINT //Defined if used UART
const struct device * uart_data_print = DEVICE_DT_GET(DT_NODELABEL(uart0));
size_t uart_buf_len = sizeof(uint8_t);
uint8_t uart_buf[255];
//Configure for UART ends


/* Use to dump n amount of buffers */
void dump_buffer_n(uint16_t *buff[], size_t len, size_t n){
    printf("\n*** START OF BUFFER DUMP ***\n");

    for(size_t i = 0; i < n; ++i){
        for(size_t j = 0; j < len / sizeof(int16_t); ++j){
            if(j % 8 == 0){ putchar('\n'); }
            printf("%04x ", buff[i][j]);
        }
        putchar('\n');
    }

    printf("\n*** END OF BUFFER DUMP ***\n\n");

}

/* Used to dump a single buffer */
void dump_buffer(uint16_t *buff, size_t len){
    printf("\n*** START OF BUFFER DUMP ***\n");
    for(size_t i = 0; i < len / sizeof(int16_t); ++i){
        if(i % 8 == 0){ putchar('\n'); }
        printf("%04x ", buff[i]);
    }

    putchar('\n');
    printf("\n*** END OF BUFFER DUMP ***\n\n");

}

void pcm_amp(int16_t *samples, size_t len, float mult){
    int16_t largest = samples[0];
    
    /* Find the largest absolute value */
    for(size_t i = 0; i < len; ++i){
        /* make sure we don't overflow on abs() */
        if(samples[i] == INT16_MIN){
            samples[i]++;
        }
        if( (abs(samples[i])) > largest ){
            largest = samples[i];
        }
    }

    float highestmult = (float)INT16_MAX / largest;

    if(mult > highestmult){
        mult = highestmult;
    }

    for(size_t i = 0; i < len; ++i){
        samples[i] = (int16_t)(samples[i] * mult);
    }
}


/* Simply switch to the next buffer unless we're already at the last buffer */
static inline uint8_t switch_buffer(uint8_t cur){
    return ( (cur + 1) >= N_BUFF ? 0 : (cur + 1) );
}

void pdm_stop(){
    nrfx_pdm_stop();
    g_pdm_stopped = 1;
}

void pdm_start(){
    g_buffsel = 0;
    nrfx_pdm_start();
    g_pdm_stopped = 0;
}

/* Event handler for pdm events
 * will be called on:
 * - error
 * - buffer required
 * - buffer full*/
void pdm_evt_handler(nrfx_pdm_evt_t const *p_evt){

    /* Release whatever data we do have and stop on error */
    if(p_evt->error != NRFX_PDM_NO_ERROR){
        LOG_ERR("PDM Overflow error %d", p_evt->error);
        pdm_stop(); 
        k_sem_give(&data_ready);
        return;
    }

    if(p_evt->buffer_requested){
        nrfx_pdm_buffer_set(g_buff[g_buffsel], (AUDIO_BLOCK_SIZE / AUDIO_SAMPLE_SIZE));
        g_buffsel = switch_buffer(g_buffsel);
    }

    if(p_evt->buffer_released != NULL){
        /* Only stop if we've reached the last buffer
         * event handler might also be called once after stopping
         * so make sure we haven't actually stopped before */
        if(p_evt->buffer_released == g_buff[N_BUFF-1]  && !g_pdm_stopped){
            pdm_stop(); 
            k_sem_give(&data_ready);
        }
    }
}


void butt_handler(uint32_t state, uint32_t has_changed){
    if(state == 1){
        pdm_start();
    }
}


void uart_evt_handler(){
    
    
    struct uart_event_rx uart_evt_read = {
        .buf = uart_buf,
        .len = uart_buf_len,
        .offset = 200,
    }; 
    


}



void serial_uart_setup(){ //Setuping UART to work

  if (!device_is_ready(uart_data_print)) {
        return;
    }

    const struct uart_config uart_conf = {
        .baudrate = 115200,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };

    //typedef void (*uart_callback_t)(uart_data_print, struct uart_event *evt, void *uart_evt_handler);

    uart_configure(uart_data_print, &uart_conf); //Configure UART

    uart_callback_set(uart_data_print,uart_evt_handler,uart_buf);

    //This part of code brokens code for working
    int error_uart_print = uart_rx_enable(uart_data_print,uart_buf,uart_buf_len,SYS_FOREVER_US); //Enabling UART receiver
    LOG_ERR("UART RX error check %d\n", error_uart_print);

    int error_uart = uart_err_check(uart_data_print); //Errors in code

    LOG_ERR("UART Error check %d\n", error_uart);

}

/*
void serial_uart_print(uint16_t show_data){ //Printing serial data to UART

    for(int i; i < 50; i++){
        uart_poll_in(uart_data_print,show_data);
        
    }
    
} 

*/

int main(){

    serial_uart_setup(); //Uart setup running

    /* TODO: see if this can be preallocated as a slab, this is a bit dirty */
    for(int i = 0; i < N_BUFF; ++i){
        g_buff[i] = calloc(AUDIO_BLOCK_SIZE, 1);
    }

    nrfx_err_t ret = 0;
    ret = dk_buttons_init(butt_handler);
    if(ret){
        LOG_ERR("Got err %d\n", ret);
    }


    nrfx_pdm_config_t pdm_cfg = NRFX_PDM_DEFAULT_CONFIG(PDM_CLK, PDM_DIN);
    pdm_cfg.mode        = NRF_PDM_MODE_MONO;
    pdm_cfg.edge        = NRF_PDM_EDGE_LEFTFALLING;
    pdm_cfg.gain_l      = NRF_PDM_GAIN_MAXIMUM;
    pdm_cfg.gain_r      = NRF_PDM_GAIN_MAXIMUM;
    pdm_cfg.clock_freq  = PDM_CLK_FREQ;
    pdm_cfg.skip_gpio_cfg = false;
    pdm_cfg.skip_psel_cfg = false;


    nrfx_pdm_init(&pdm_cfg, pdm_evt_handler);
    /* This *MUST* be called after nrfx_pdm_init or configuration will be overwritten */
    set_pdm_ratio(PDM_RATIO);

    for(;;){
        k_sem_take(&data_ready, K_FOREVER);
        
        for(size_t i = 0; i < N_BUFF; ++i){
            pcm_amp(g_buff[i], AUDIO_BLOCK_SIZE, PCM_AMP);
        }

        #ifdef UART_PRINT
        //serial_uart_print(g_buff); //Print serial data in UART
        #endif
    
        dump_buffer_n((uint16_t**)g_buff, AUDIO_BLOCK_SIZE, N_BUFF);
    }

    for(int i = 0; i < N_BUFF; ++i){
        free(g_buff[i]);
    }

    return 0;
}
