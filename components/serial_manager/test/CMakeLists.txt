# On enregistre ce dossier comme un composant de test
idf_component_register(SRCS "test_serial_manager.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES serial_manager unity alert_manager)
