
enum paging_signal;

enum sound_direction {
	SOUND_DIR_PLAY,
	SOUND_DIR_REC,
	SOUND_DIR_DUPLEX,
};

void *sound_open(int direction, const char *audiodev, double *tx_frequency, double *rx_frequency, int *am, int channels, double paging_frequency, int samplerate, int buffer_size, double interval, double max_deviation, double max_modulation, double modulation_index);
int sound_start(void *inst);
void sound_close(void *inst);
int sound_write(void *inst, sample_t **samples, uint8_t **power, int num, enum paging_signal *paging_signal, int *on, int channels);
int sound_read(void *inst, sample_t **samples, int num, int channels, double *rf_level_db);
int sound_get_tosend(void *inst, int buffer_size);
int sound_is_stereo_capture(void *inst);
int sound_is_stereo_playback(void *inst);

