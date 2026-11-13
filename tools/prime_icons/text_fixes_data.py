"""Corrections for the localized text of the USA discs (RM3E01 and Trilogy R3ME01), read by text_fixes.py.

Everything here was written for the port. The European discs (RM3P01, R3MP01) were only used to find
what was wrong; nothing is copied from them, and the runtime never reads them. Official names of
places and creatures are kept as each language calls them.

Three kinds of rules, per language (GERM, FREN, SPAN, ITAL):
- FRAGMENTS: a short fragment and its correction, applied to every string of that language where the
  fragment appears at word boundaries.
- SCOPED: the same, limited to one string ("ASSETID#index") or one table ("ASSETID").
- WHOLE: the string "ASSETID#index" is replaced as a whole, but only if it contains the short guard
  (so another disc revision with a different text at that index is left alone). A plain text keeps
  the original's leading formatting tags (&just=center; ...) and final newline; ('raw', text) is used
  as written.
"""

FRAGMENTS = {
    'SPAN': [
        # Left in English.
        ('Red Phaazoid&pop;', 'Phaazoide carmesí&pop;'),
        ('Phazite Armored Trooper&pop;', 'Soldado de phazita&pop;'),
        ('&just=left;la armadura de Phazita', '&just=left;La armadura de phazita'),
        ('INFORMATION:', 'DATOS:'),
        ('Punto de Enganche Point', 'Punto de Enganche'),
        ('al Stick de Control', 'la Palanca de Control'),
        ('el Stick de Control', 'la Palanca de Control'),
        ('Toggle Bookmark', 'Cambiar marcador'),
        ('Mounted Blast Shield', 'Escudo fijo'),
        ('Planet Tallon IV', 'Planeta Tallon IV'),
        ('Heavy Galactic Federation Crate', 'Contenedor pesado de la Federación'),
        ('&space;Restore Default&space;', '&space;Valores predeterminados&space;'),
        ('[ Bonus Gallery ]', '[ Galería de extras ]'),
        ('&space;Locked&pop;', '&space;Bloqueado&pop;'),
        ('&space;Stop&endlink;', '&space;Parar&endlink;'),
        ('&space;Play&endlink;', '&space;Oír&endlink;'),
        ('&space;View&endlink;', '&space;Ver&endlink;'),
        # Typos and missing accents.
        ('via satélite', 'vía satélite'), ('Phazee', 'Phaaze'), ('Bryo', 'Bryyo'), ('Bryonnis', 'Bryyonnis'),
        ('Cieloab', 'Cielolab'), ('Elyssia', 'Elysia'), ('Gohr', 'Ghor'), ('Pazhita', 'Phazita'),
        ('laviatán', 'leviatán'), ('extramadamente', 'extremadamente'), ('enegía', 'energía'),
        ('ultraresistente', 'ultrarresistente'), ('repitilicus', 'reptilicus'), ('faacultad', 'facultad'),
        ('arrrogante', 'arrogante'), ('solitatario', 'solitario'), ('kinéticos', 'cinéticos'),
        ('sobreexposión', 'sobreexposición'), ('alimeta', 'alimenta'), ('dfue', 'fue'), ('brites', 'brotes'),
        ('almecenamiento', 'almacenamiento'), ('ujna', 'una'), ('no stá', 'no está'), ('anteriosres', 'anteriores'),
        ('permanerce', 'permanecer'), ('espacieles', 'espaciales'), ('enfretamiento', 'enfrentamiento'),
        ('confirmarion', 'confirmaron'), ('camarotas', 'camarotes'), ('regaloGift', 'regalo'), ('vuelte', 'vuelve'),
        ('cominucarse', 'comunicarse'), ('informacióon', 'información'), ('ontener', 'obtener'),
        ('accededar', 'acceder'), ('punte', 'puente'), ('desvastador', 'devastador'), ('Puerte', 'Puerta'),
        ('puerte', 'puerta'), ('suelen mostran', 'suelen mostrar'), ('envia', 'envía'), ('golem', 'gólem'),
        ('Golem', 'Gólem'), ('Golems', 'Gólems'), ('La bese', 'La base'), ('Barrra', 'Barra'),
        ('las arms de', 'las armas de'), ('conctada', 'conectada'), ('rampla', 'rampa'), ('macanismo', 'mecanismo'),
        ('sindica', 'indica'), ('la victima', 'la víctima'), ('extraido', 'extraído'), ('concta', 'conecta'),
        ('electricos', 'eléctricos'), ('regista', 'registra'), ('energénicas', 'energéticas'),
        ('abjetivos', 'objetivos'), ('cuénto', 'cuánto'), ('Defiendela', 'Defiéndela'), ('la NF Olimpo', 'la NFG Olimpo'),
        ('acabo cayendo', 'acabó cayendo'), ('se estrello', 'se estrelló'), ('600 llego', '600 llegó'),
        ('ciencia dejo de', 'ciencia dejó de'), ('tendríamos accedo', 'tendríamos acceso'),
        ('el informa', 'el informe'), ('este informa', 'este informe'), ('Informa de estado', 'Informe de estado'),
        ('por método convencionales', 'por métodos convencionales'),
    ],
    'FREN': [
        ('Gragnol Adult', 'Gragnol adulte'),
        ('complétement', 'complètement'), ('recommendé', 'recommandé'), ('recommendée', 'recommandée'),
        ('recommendées', 'recommandées'), ('Puit de', 'Puits de'), ('miliers', 'milliers'), ('organsime', 'organisme'),
        ("d'arbordage", "d'abordage"), ('attibuts', 'attributs'), ('souffre&pop;', 'soufre&pop;'),
        ("piel d'énergie", "pile d'énergie"), ('inconu', 'inconnu'), ('la scienc,', 'la science,'),
        ("J'ai construis", "J'ai construit"), ('succés', 'succès'), ('developpement', 'développement'),
        ('poursuivimes', 'poursuivîmes'), ('traîté', 'traité'), ('platefrome', 'plateforme'), ('Cèlestius', 'Célestius'),
        ('sécuríté', 'sécurité'), ('ingénieurie', 'ingénierie'), ("jusqu'á", "jusqu'à"), ('les cables', 'les câbles'),
        ('retractée', 'rétractée'), ('Ceci représenté', 'Ceci représente'), ('Il ne nous fallu', 'Il ne nous fallut'),
        ("en s'adaptant", "en m'adaptant"), ('détruisants', 'détruisant'),
        ('peuvent être soulevé par', 'peuvent être soulevés par'), ("avons été témoin d'", "avons été témoins d'"),
    ],
    'GERM': [
        ('Morphology:', 'Morphologie:'), ('Extras-Menu', 'Extras-Menü'), ('Piraten-Hussar', 'Piraten-Husar'),
        ('Hussaren', 'Husaren'), ('Aktualisierte Statusbericht', 'Aktualisierter Statusbericht'),
        ('Metroid-Project', 'Metroid-Projekt'),
        ('Verteidgungssystems', 'Verteidigungssystems'), ('Verteidgungssysteme', 'Verteidigungssysteme'),
        ('Voteil', 'Vorteil'), ('erinneren', 'erinnern'), ('ausgreift', 'ausgereift'), ('Luftwabwehr', 'Luftabwehr'),
        ('Überadung', 'Überladung'), ('Phazooid', 'Phaazoid'), ('Materialen', 'Materialien'), ('Föderarion', 'Föderation'),
        ('großere', 'größere'), ('vorrüber', 'vorüber'), ('vorrübergehend', 'vorübergehend'),
        ('Verletzungsfgefahr', 'Verletzungsgefahr'), ('Ebenblider', 'Ebenbilder'), ('Korrakks', 'Korakks'),
        ('emfindlich', 'empfindlich'), ('konstuiert', 'konstruiert'), ('ein Schwam', 'ein Schwarm'),
        ('Aglität', 'Agilität'), ('widerstandfähig', 'widerstandsfähig'), ('Offensichtilich', 'Offensichtlich'),
        ('Enwicklungsteams', 'Entwicklungsteams'), ('gifitge', 'giftige'), ('morpende', 'morphende'),
        ('Bedrophung', 'Bedrohung'), ('Degenaration', 'Degeneration'), ('Kriminiellen', 'Kriminellen'),
        ('Erfassungsystem', 'Erfassungssystem'), ('Charaktersitk', 'Charakteristik'), ('im Nachkampf', 'im Nahkampf'),
        ('von unschätzbaren Wert', 'von unschätzbarem Wert'), ('hochentwicklte', 'hochentwickelte'),
        ('widerstandsfähgien', 'widerstandsfähigen'), ('duch', 'durch'),
        ('mit hochfrequentem Beam-Waffen', 'mit hochfrequenten Beam-Waffen'), ('Exemplate', 'Exemplare'),
        ('Satelliteninformation zeigen', 'Satelliteninformationen zeigen'), ('Galactische', 'Galaktische'),
        ('diesen Rückschlagen', 'diesen Rückschlägen'), ('in die Barberei', 'in die Barbarei'), ('Loyaliät', 'Loyalität'),
        ('Kampfapells', 'Kampfappells'), ('nichts entgegensetzen', 'nichts entgegenzusetzen'),
        ('radioktives', 'radioaktives'), ('Freudesgesängen', 'Freudengesängen'), ('Mermale', 'Merkmale'),
        ('gößte', 'größte'), ('föderierten Freunden bei', 'föderierten Freunde bei'),
        ('Observationssatteliten', 'Observationssatelliten'), ('Postionen', 'Positionen'),
        ('als Kopfjäger', 'als Kopfgeldjäger'), ('Idividuums', 'Individuums'), ('Traningsdrohnen', 'Trainingsdrohnen'),
        ('Wilkommen', 'Willkommen'), ('Übebrückung', 'Überbrückung'), ('Wrack einers', 'Wrack eines'),
        ('Austretrendes', 'Austretendes'), ('voll auslastet', 'voll ausgelastet'), ('Waffenystem', 'Waffensystem'),
        ('erneut aktiveren', 'erneut aktivieren'), ('Fließgeschwindgkeit', 'Fließgeschwindigkeit'),
        ('Errrichtung', 'Errichtung'), ('Stillegung', 'Stilllegung'), ('angrenzendas', 'angrenzenden'),
        ('Expresss', 'Expresses'), ('ziwschen', 'zwischen'), ('Pilzssporen', 'Pilzsporen'), ('übermittlen', 'übermitteln'),
        ('Rundass', "Rundas'"), ('Magnetischiene', 'Magnetschiene'), ('ausgerichtetet', 'ausgerichtet'),
        ('Seite zu kampfen', 'Seite zu kämpfen'), ('weich Körpermasse', 'weiche Körpermasse'),
        ('Gemäß der Sicherheitsprotokoll', 'Gemäß dem Sicherheitsprotokoll'),
    ],
    'ITAL': [
        # Left in English.
        ('Multi-Lock Blast Shield', 'Scudo Raggi multiblocco'), ('Mounted Blast Shield', 'Scudo Raggi fisso'),
        ('Zipline Cable', 'Cavo della Funivia'),
        ('Data moved to Logbook for review.', 'Dati scaricati nel Registro per ulteriore consultazione.'),
        ('&else;Energy Cell&endif;', '&else;Nucleo Energia&endif;'), ('Vehicle: T.T.C.', 'Veicolo: T.T.C.'),
        ('Cavern Metroid', 'Grotta Metroid'), ('Press&image=', 'Premi &image='),
        ('Tieni premuto Hold &image', 'Tieni premuto &image'),
        ('&space;Stop&endlink;', '&space;Ferma&endlink;'), ('&space;Play&endlink;', '&space;Ascolta&endlink;'),
        # Typos.
        ('antiaree', 'antiaeree'), ('Phazee', 'Phaaze'), ('porterla', 'poterla'), ('La boma', 'La bomba'),
        ('Perquisicilo', 'Perquisiscilo'), ('acciacio', 'acciaio'), ('Il terminal controlla', 'Il terminale controlla'),
        ('Nuogolobot', 'Nugolobot'), ('riempendo', 'riempiendo'), ('immagazinato', 'immagazzinato'),
        ('immagazinata', 'immagazzinata'), ('insaguinato', 'insanguinato'), ('primative', 'primitive'),
        ('sono statti', 'sono stati'), ('retrattitili', 'retrattili'), ('sufficiente pe distruggerlo', 'sufficiente per distruggerlo'),
        ('parete rocciose', 'parete rocciosa'), ('chiuoa', 'chiuso'), ('Multi torretta', 'Multitorretta'),
        ("per l'AU.", "per l'UA."),
    ],
}

# Trilogy's menu (fe/GuiDVD.PAK, table 3F89B06D5E684C6A) was left mostly in English in every language.
_MENU = '3F89B06D5E684C6A'
_MENU_NAMES = [  # English, German, French, Spanish, Italian
    ('[ Play Game ]', '[ Spielen ]', '[ Jouer ]', '[ Continuar partida ]', '[ Gioca ]'),
    ('[ Start Game ]', '[ Spiel beginnen ]', '[ Commencer ]', '[ Empezar partida ]', '[ Inizia a giocare ]'),
    ('[ Play Single Player ]', '[ Einzelspieler ]', '[ Un joueur ]', '[ Un jugador ]', '[ Giocatore singolo ]'),
    ('[ Start Single Player ]', '[ Einzelspieler beginnen ]', '[ Commencer en solo ]', '[ Empezar Un jugador ]',
     '[ Inizia giocatore singolo ]'),
    ('[ Play MP2: Echoes Multiplayer ]', '[ MP2: Echoes - Mehrspieler ]', '[ MP2: Echoes - Multijoueur ]',
     '[ MP2: Echoes - Multijugador ]', '[ MP2: Echoes - Multigiocatore ]'),
    ('Select game to be erased…', 'Welche Datei soll gelöscht werden?', 'Choisissez la sauvegarde à effacer.',
     '¿Qué partida quieres borrar?', 'Scegli la partita da cancellare.'),
    ('Single Player Games', 'Einzelspieler-Spielstände', 'Parties en solo', 'Partidas de Un jugador',
     'Partite giocatore singolo'),
    ('[ Back ]', '[ Zurück ]', '[ Retour ]', '[ Atrás ]', '[ Indietro ]'),
    ('[ Erase ]', '[ Löschen ]', '[ Effacer ]', '[ Borrar ]', '[ Cancella ]'),
    ('&space;Locked&pop;', '&space;Gesperrt&pop;', '&space;Bloqué&pop;', '&space;Bloqueado&pop;', '&space;Bloccato&pop;'),
    ('&space;On&endlink;', '&space;An&endlink;', '&space;Activé&endlink;', '&space;Sí&endlink;', '&space;Sì&endlink;'),
    ('&space;Off&endlink;', '&space;Aus&endlink;', '&space;Désactivé&endlink;', '&space;No&endlink;', '&space;No&endlink;'),
    ('&space;Unlocked&endif;', '&space;Freigeschaltet&endif;', '&space;Débloqué&endif;', '&space;Desbloqueado&endif;',
     '&space;Sbloccato&endif;'),
    ('Music from', 'Musik aus', 'Musique de', 'Música de', 'Musiche di'),
    ('Use the Nunchuk Control Stick to scroll list', 'Bewege den Control Stick des Nunchuks, um durch die Liste zu blättern.',
     'Utilisez le Stick du Nunchuk pour faire défiler la liste.',
     'Usa la palanca de control del Nunchuk para desplazarte por la lista.',
     'Usa il Control Stick del Nunchuk per scorrere la lista.'),
    ('MP1 - World Gallery', 'MP1 - Welten', 'MP1 - Galerie des mondes', 'MP1 - Galería de mundos', 'MP1 - Galleria dei mondi'),
    ('MP1 - Biology Gallery', 'MP1 - Lebensformen', 'MP1 - Galerie biologique', 'MP1 - Galería de biología',
     'MP1 - Galleria biologica'),
    ('MP1 - Samus Gallery', 'MP1 - Samus', 'MP1 - Galerie de Samus', 'MP1 - Galería de Samus', 'MP1 - Galleria di Samus'),
    ('MP1 - Extra Gallery', 'MP1 - Extras', 'MP1 - Galerie bonus', 'MP1 - Galería de extras', 'MP1 - Galleria extra'),
    ('MP2 - Promo Art Package', 'MP2 - Werbegrafiken', 'MP2 - Art promotionnel', 'MP2 - Arte promocional',
     'MP2 - Arte promozionale'),
    ('MP2 - Character Package', 'MP2 - Charaktere', 'MP2 - Personnages', 'MP2 - Personajes', 'MP2 - Personaggi'),
    ('MP2 - Creature Package', 'MP2 - Kreaturen', 'MP2 - Créatures', 'MP2 - Criaturas', 'MP2 - Creature'),
    ('MP2 - Boss Package', 'MP2 - Bosse', 'MP2 - Boss', 'MP2 - Jefes', 'MP2 - Boss'),
    ('MP2 - Sketch Package', 'MP2 - Skizzen', 'MP2 - Croquis', 'MP2 - Bocetos', 'MP2 - Bozzetti'),
    ('MP2 - Storyboard Package', 'MP2 - Storyboards', 'MP2 - Storyboards', 'MP2 - Guiones gráficos', 'MP2 - Storyboard'),
    ('MP1 Fusion Suit', 'MP1 - Fusion Suit', 'MP1 - Costume Fusion', 'MP1 - Traje Fusión', 'MP1 - Tuta Fusione'),
    ('Metroid Prime Theme', 'Metroid Prime - Thema', 'Thème de Metroid Prime', 'Tema de Metroid Prime',
     'Tema di Metroid Prime'),
    ('Parasite Queen', 'Mutterparasit', 'Reine parasite', 'Parásito Reina', 'Parassita Regina'),
    ('Tallon Overworld', 'Tallon-Oberwelt', 'Surface de Tallon', 'Superficie de Tallon', 'Superficie di Tallon'),
    ('Chozo Shrine', 'Chozo-Tempel', 'Temple Chozo', 'Templo Chozo', 'Santuario Chozo'),
    ('Chozo Ruins', 'Chozo-Ruinen', 'Ruines Chozo', 'Ruinas Chozo', 'Rovine Chozo'),
    ('Flaaghra', 'Flaahgra', 'Flaahgra', 'Flaahgra', 'Flaahgra'),
    ('Magmoor Caverns', 'Höhlen von Magmoor', 'Cavernes du Magmoor', 'Cavernas Magmoor', 'Grotte Magmoor'),
    ('Phendrana Drifts', 'Eiswüste von Phendrana', 'Monts de Phendrana', 'Phendrana', 'Phendrana'),
    ('Phendrana Battle', 'Kampf in Phendrana', 'Combat dans Phendrana', 'Combate en Phendrana', 'Battaglia a Phendrana'),
    ('Crashed Ship', 'Havarierte Fregatte', 'Frégate échouée', 'Nave estrellada', 'Fregata precipitata'),
    ('Phazon Mines', 'Phazon-Minen', 'Mines de Phazon', 'Minas de Phazon', 'Miniere di Phazon'),
    ('Space Pirates', 'Weltraumpiraten', "Pirates de l'espace", 'Piratas espaciales', 'Pirati Spaziali'),
    ('Omega Pirate', 'Omegapirat', 'Pirate oméga', 'Pirata Omega', 'Pirata Omega'),
    ('Meta Ridley', 'Meta-Ridley', 'Méta Ridley', 'Meta Ridley', 'Meta Ridley'),
    ('Metroid Prime Battle', 'Kampf gegen Metroid Prime', 'Combat contre Metroid Prime', 'Combate contra Metroid Prime',
     'Battaglia contro Metroid Prime'),
    ('Metroid Prime Credits', 'Metroid Prime - Abspann', 'Générique de Metroid Prime', 'Créditos de Metroid Prime',
     'Titoli di coda di Metroid Prime'),
    ('Metroid Prime 2: Echoes Theme', 'Metroid Prime 2: Echoes - Thema', 'Thème de Metroid Prime 2: Echoes',
     'Tema de Metroid Prime 2: Echoes', 'Tema di Metroid Prime 2: Echoes'),
    ('Splinter Hive', 'Splinter-Bau', 'Ruche de Splinters', 'Colmena de esquirlas', 'Nido degli Splinter'),
    ('Dark Trooper', 'Dunkler Soldat', 'Soldat Sombre', 'Soldado oscuro', 'Soldato Oscuro'),
    ('Luminoth Temple', 'Tempel der Luminoth', 'Temple Luminoth', 'Templo luminario', 'Tempio Luminoth'),
    ('The Luminoth', 'Die Luminoth', 'Les Luminoth', 'Los Luminarios', 'I Luminoth'),
    ('Dark Temple', 'Dunkler Tempel', 'Temple Sombre', 'Templo Oscuro', 'Tempio Oscuro'),
    ('Dark Agon Wastes', 'Ödland vom dunklen Agon', "Désert d'Agon Sombre", 'Yermos de Agón Oscuro',
     'Deserto di Agon Oscuro'),
    ('Agon Wastes', 'Ödland von Agon', "Désert d'Agon", 'Yermos de Agón', 'Deserto di Agon'),
    ('Torvus Catacombs', 'Torvus-Katakomben', 'Catacombes de Torvus', 'Catacumbas de Torvus', 'Catacombe di Torvus'),
    ('Ing Battle', 'Kampf gegen die Ing', 'Combat contre les Ing', 'Combate contra los Oscuros', 'Battaglia contro gli Ing'),
    ('Sanctuary Fortress', 'Geheiligte Festung', 'Forteresse du Sanctuaire', 'Fortaleza del Santuario',
     'Fortezza del Santuario'),
    ('Dark Samus', 'Dunkle Samus', 'Samus Sombre', 'Samus Oscura', 'Samus Oscura'),
    ('MP3: Corruption Theme', 'MP3: Corruption - Thema', 'Thème de MP3: Corruption', 'Tema de MP3: Corruption',
     'Tema di MP3: Corruption'),
    ('Aurora Unit 313', 'Aurora-Einheit 313', 'Unité Aurora 313', 'Unidad Aurora 313', 'Unità Aurora 313'),
    ('MP1 Track', 'MP1 - Stück', 'MP1 - Piste', 'MP1 - Pista', 'MP1 - Brano'),
    ('MP2 Track', 'MP2 - Stück', 'MP2 - Piste', 'MP2 - Pista', 'MP2 - Brano'),
    ('MP3 Track', 'MP3 - Stück', 'MP3 - Piste', 'MP3 - Pista', 'MP3 - Brano'),
    ('File 1&endif;', 'Datei 1&endif;', 'Fichier 1&endif;', 'Archivo 1&endif;', 'File 1&endif;'),
    ('File 2&endif;', 'Datei 2&endif;', 'Fichier 2&endif;', 'Archivo 2&endif;', 'File 2&endif;'),
    ('File 3&endif;', 'Datei 3&endif;', 'Fichier 3&endif;', 'Archivo 3&endif;', 'File 3&endif;'),
    ('&else;Slot 1&endif;', '&else;Speicherplatz 1&endif;', '&else;Emplacement 1&endif;', '&else;Partida 1&endif;',
     '&else;Slot 1&endif;'),
    ('&else;Slot 2&endif;', '&else;Speicherplatz 2&endif;', '&else;Emplacement 2&endif;', '&else;Partida 2&endif;',
     '&else;Slot 2&endif;'),
    ('&else;Slot 3&endif;', '&else;Speicherplatz 3&endif;', '&else;Emplacement 3&endif;', '&else;Partida 3&endif;',
     '&else;Slot 3&endif;'),
    ('Select Level', 'Level wählen', 'Sélectionnez un niveau', 'Selecciona un nivel', 'Seleziona livello'),
    ('[ GF Ship ]', '[ Schiff der GF ]', '[ Vaisseau de la FG ]', '[ Nave de la FG ]', '[ Nave della FG ]'),
    ('[ Skytown ]', '[ Himmelsstadt ]', '[ Célestius ]', '[ Cielolab ]', '[ Etra ]'),
]
_LANGS = ('GERM', 'FREN', 'SPAN', 'ITAL')
SCOPED = {lang: [] for lang in _LANGS}
for english, *texts in _MENU_NAMES:
    for lang, text in zip(_LANGS, texts):
        if text != english:
            SCOPED[lang].append((_MENU, english, text))
# Only the German logbook entry for Aurora Unit 242 has another enemy's name as its title.
SCOPED['GERM'].append(('51CF1C10341DCA62#2', 'Advanced Aerotrooper&pop;', 'Aurora-Einheit 242&pop;'))

_FUSION = {  # Trilogy menu: activate / purchase the Fusion Suit (fe/GuiDVD.PAK FA53C4BF3E3641C6 #2, #6)
    'GERM': ('Fusion Suit aktivieren? Samus trägt dann in MP1 den Fusion Suit.',
             'Fusion Suit erwerben? Samus trägt dann in MP1 den Fusion Suit.'),
    'FREN': ('Activer le costume Fusion? Samus portera le costume Fusion dans MP1.',
             'Acheter le costume Fusion? Samus portera le costume Fusion dans MP1.'),
    'SPAN': ('¿Quieres activar el Traje Fusión? Samus lo llevará puesto en MP1.',
             '¿Quieres comprar el Traje Fusión? Samus lo llevará puesto en MP1.'),
    'ITAL': ('Vuoi attivare la Tuta Fusione? Samus la indosserà in MP1.',
             'Vuoi acquistare la Tuta Fusione? Samus la indosserà in MP1.'),
}
_NETWORK = {  # Parental controls notice (Trilogy MP3 MiscData F26B5C2F9FF2B96E #0, menu FA53C4BF3E3641C6 #100)
    'GERM': 'Die Nutzung der Netzwerkfunktionen ist eingeschränkt. Bitte überprüfe die Einstellungen der Altersbeschränkungen in den Wii-Optionen.',
    'FREN': "L'utilisation des fonctions réseau est restreinte. Veuillez vérifier les paramètres du contrôle parental dans les Paramètres Wii.",
    'SPAN': 'El uso de las funciones de red está restringido. Revisa la configuración del control parental en las Opciones de Wii.',
    'ITAL': "L'uso delle funzioni di rete è limitato. Controlla le impostazioni del Filtro famiglia nelle Opzioni Wii.",
}
_BOARD = {  # Wii Message Board notices (Trilogy MiscData 19E67614C483231C #0: taking; 485D00CEA1713040 #220: posting)
    'GERM': ('Damit die Wii-Pinnwand nicht zu schnell voll wird, mache nicht zu viele Bildschirmfotos.',
             'Damit die Wii-Pinnwand nicht zu schnell voll wird, veröffentliche nicht zu viele Bildschirmfotos.'),
    'FREN': ("Pour que le bureau Wii ne se remplisse pas trop vite, évitez de prendre trop de captures d'écran.",
             "Pour que le bureau Wii ne se remplisse pas trop vite, évitez de publier trop de captures d'écran."),
    'SPAN': ('Para que el tablón de Wii no se llene demasiado rápido, evita hacer demasiadas capturas de pantalla.',
             'Para que el tablón de Wii no se llene demasiado rápido, evita publicar demasiadas capturas de pantalla.'),
    'ITAL': ('Per evitare che la bacheca Wii si riempia troppo in fretta, non catturare troppe immagini.',
             'Per evitare che la bacheca Wii si riempia troppo in fretta, non pubblicare troppe immagini.'),
}
_OLDER = "Questo file appartiene a una versione precedente del gioco. Se non lo elimini, il gioco potrebbe non funzionare correttamente."

WHOLE = {lang: [] for lang in _LANGS}
for lang in _LANGS:
    activate, purchase = _FUSION[lang]
    WHOLE[lang] += [
        ('FA53C4BF3E3641C6#2', 'Activate Fusion Suit', activate),
        ('FA53C4BF3E3641C6#6', 'Purchase Fusion Suit', purchase),
        ('FA53C4BF3E3641C6#100', 'Use of network features', _NETWORK[lang]),
        ('F26B5C2F9FF2B96E#0', 'Use of network features', _NETWORK[lang]),
        ('19E67614C483231C#0', 'Wii Message Board', _BOARD[lang][0]),
        ('485D00CEA1713040#220', 'Wii Message Board', _BOARD[lang][1]),
    ]
WHOLE['SPAN'].append(('1E249B9A4943F8BE#0', 'The blast doors are down',
                      'Gracias por tu ayuda, Samus. Las compuertas blindadas se han cerrado, así que tendrás que buscar '
                      'otro camino para volver a tu nave.'))
_MULTI_LOCK = ('raw', '&push;&main-color=#E67E00FF;Scudo Raggi multiblocco&pop;\n&just=left;Questo tipo di Scudo Raggi è '
               'dotato di più meccanismi di chiusura. Per disattivarli e rimuovere lo scudo è necessario colpire tutti '
               'i bersagli contemporaneamente.')
WHOLE['ITAL'] += [
    ('6204C3BC14C576F0#2', 'This type of Blast Shield', _MULTI_LOCK),
    ('F55D2C75CB1B9AEE#1', 'This type of Blast Shield', _MULTI_LOCK),
    ('F4BA0BF91013A3FF#2', 'Mounted Blast Shields are', (
        'raw', '&push;&main-color=#E67E00FF;Scudo Raggi fisso&pop;\n&just=left;Gli Scudi Raggi fissi si distinguono '
        'perché spesso dispongono di un sistema di chiusura principale, di solito formato da più blocchi di sgancio '
        'da colpire in sequenza. Una volta disattivato il sistema di chiusura, lo scudo può essere rimosso. I colpi '
        'delle armi non bastano a danneggiarlo, ma si può strappare facilmente con una trazione decisa.')),
    ('EFABF984AFAF691C#0', 'is drained of power', ('raw', "Il terminale è stato privato dell'energia.\nLo scudo vicino "
                                                    "è stato disattivato.\nL'unità non può più essere usata.")),
    ('F82D90109A7F1484#9', 'Choosing to save now', (
        'raw', "Se salvi ora, questa partita di\nMetroid Prime 3: Corruption terminerà.\nQuando selezionerai di nuovo "
               "lo slot,\ninizierai una nuova partita.\n\nVuoi salvare?")),
    ('F82D90109A7F1484#2', 'older version of the game', _OLDER),
    ('E92087CA58AB7B2C#2', 'older version of the game', _OLDER),
    ('485D00CEA1713040#162', 'Are you sure?', 'Sei sicuro?'),
]

# Trilogy's Extras gallery hint (MP3/GuiDVD.PAK 4B58B2CD773428CD #103), English in every language.
for _lang, _text in (('GERM', 'Bewege den Stick des Nunchuks, um durch die Liste zu blättern.'),
                     ('FREN', 'Utilisez le Stick du Nunchuk pour faire défiler la liste.'),
                     ('SPAN', 'Usa la palanca del Nunchuk para desplazarte por la lista.'),
                     ('ITAL', 'Usa la levetta del Nunchuk per scorrere la lista.')):
    SCOPED[_lang].append(('4B58B2CD773428CD#103', 'Use the Nunchuk stick to scroll list', _text))

# Corruption: wrong button or reward icons, and a missing &push; (checked against the English and the
# other languages of the same string).
SCOPED['ITAL'] += [
    ('109661868C439384#0', '&image=0xE8D9FB7A41321CE6;', '&image=0x669DE4B24CC08FFF;'),  # gold credit -> voucher
    ('8F2188F416883AC4#0', '&image=0x5FC17B1F30BAA7AE;', '&image=0xF4DCD3D318C1156C;'),  # B -> the prompt's icon
]
SCOPED['SPAN'].append(('82A0F6A1919DA5C0#0', '&image=0xF76434386E7D8B5C;Verduga', 'Verduga'))  # extra friend credit icon
SCOPED['GERM'].append(('A97384659755B60B#0', '&main-color=#B3B300;schimmernde&pop; Objekte erfassen.',
                       '&push;&main-color=#B3B300;schimmernde&pop; Objekte zu erfassen.'))

# Trilogy MP3 scans added for the Wii release, English in every language on the USA disc (the European
# disc carries translated copies in RS5.PAK). Keys: STRG asset #0; guard: the English start.
_SCANS = [
    ('2A8DEBC8931BDFDC', 'Ship Command Received',
     'Schiffsbefehl empfangen.\nFracht wird an ihren Ursprungsort\nzurückgebracht, neuer\nSchiffsbefehl wird ausgeführt.',
     "Commande du vaisseau reçue.\nRetour de la cargaison à son\nemplacement d'origine et exécution\nde la nouvelle commande.",
     'Orden para la nave recibida.\nDevolviendo la carga a su\nubicación original y ejecutando\nla nueva orden.',
     "Comando della nave ricevuto.\nIl carico torna alla posizione\noriginale e viene eseguito il\nnuovo comando."),
    ('EB553B2FCADE850E', 'Door system connected to',
     'Türsystem mit dem Hauptverteiler\ndieses Raums verbunden.\nVerteiler derzeit außer Betrieb.',
     "Système de porte relié au\ndistributeur principal de cette salle.\nDistributeur actuellement hors service.",
     'Sistema de la puerta conectado al\ndistribuidor principal de esta sala.\nDistribuidor desactivado.',
     "Sistema della porta collegato al\ndistributore principale di questa sala.\nDistributore al momento disattivato."),
    ('C8BF092ED9F05937', 'Door Shield engaged',
     'Türschild wegen Energieverlust\naktiviert. Verteiler mit Energie\nversorgen, um die Sperre zu öffnen.',
     "Bouclier de porte activé suite à\nune perte d'énergie. Alimentez le\ndistributeur pour le déverrouiller.",
     'Escudo de la puerta activado por\npérdida de energía. Restablece la energía\ndel distribuidor para abrirlo.',
     "Scudo della porta attivato per\nperdita di energia. Ripristina l'energia\ndel distributore per sbloccarlo."),
    ('BB015C960F0BB519', 'Circuit inside projector',
     'Schaltkreis im Projektor repariert.\nProjektor über die Steuereinheit\naktivieren.',
     "Circuit du projecteur réparé.\nUtilisez l'unité de contrôle\npour activer le projecteur.",
     'Circuito del proyector reparado.\nUsa la unidad de control\npara activar el proyector.',
     "Circuito del proiettore riparato.\nUsa l'unità di controllo\nper attivare il proiettore."),
    ('012FA97C870E45A2', 'Primary lift system online',
     'Hauptaufzugssystem aktiv.\nZugang zum Lastenaufzug gestattet.\nHologramm betreten, um den Aufzug zu starten.',
     "Système d'ascenseur principal opérationnel.\nAccès au monte-charge autorisé.\nEntrez dans l'hologramme pour l'activer.",
     'Sistema de elevación principal activo.\nAcceso al montacargas permitido.\nEntra en el holograma para activarlo.',
     "Sistema di sollevamento primario attivo.\nAccesso al montacarichi consentito.\nEntra nell'ologramma per attivarlo."),
    ('68F1224332D24769', 'Terminal powered down',
     'Terminal abgeschaltet.\nZugang zum nahen Aufzug\nist jetzt gestattet.',
     "Terminal hors tension.\nAccès à l'ascenseur voisin\nautorisé.",
     'Terminal apagado.\nYa se puede acceder al\nascensor cercano.',
     "Terminale spento.\nOra è possibile accedere\nall'ascensore vicino."),
    ('D7B207E0D4F96236', 'Elevator activated',
     'Aufzug aktiviert.\nSteuerkonsole des Aufzugs\nist jetzt außer Betrieb.',
     "Ascenseur activé.\nLa console de contrôle de\nl'ascenseur est hors service.",
     'Ascensor activado.\nLa consola de control del\nascensor ya no funciona.',
     "Ascensore attivato.\nLa console di controllo\ndell'ascensore è disattivata."),
    ('956ED43FE086507D', 'Control console has',
     'Steuerkonsole hat die\nZugangstüren geöffnet. Konsole\nkann nicht mehr benutzt werden.',
     "La console de contrôle a ouvert\nles portes d'accès. Elle ne peut\nplus être utilisée.",
     'La consola de control ha abierto\nlas puertas de acceso. Ya no\nse puede volver a usar.',
     "La console di controllo ha aperto\nle porte di accesso. Non può\npiù essere usata."),
    ('546D86FC8BC13AFD', 'Conveyor redirection',
     'Umleitung des Förderbands abgeschlossen.\nSteuerterminal kann nicht\nmehr benutzt werden.',
     "Redirection du convoyeur terminée.\nLe terminal de contrôle ne peut\nplus être utilisé.",
     'Desvío de la cinta transportadora completado.\nEl terminal de control ya no\nse puede usar.',
     "Deviazione del nastro trasportatore completata.\nIl terminale di controllo non può\npiù essere usato."),
    ('DEAB0BEF1E383D3F', 'Terminal has disengaged\nenergy core',
     'Terminal hat die Schilde des\nEnergiekerns deaktiviert und kann\nnicht mehr benutzt werden.',
     "Le terminal a abaissé les boucliers\ndu noyau d'énergie et ne peut\nplus être utilisé.",
     'El terminal ha desactivado los\nescudos del núcleo de energía y\nya no se puede usar.',
     "Il terminale ha disattivato gli\nscudi del Nucleo Energia e non\npuò più essere usato."),
    ('45803FF59FB6A7B3', 'Terminal has disengaged\nnearby',
     'Terminal hat den nahen\nEnergieschild deaktiviert. Terminal\nkann nicht mehr benutzt werden.',
     "Le terminal a désactivé le bouclier\nd'énergie voisin. Il ne peut\nplus être utilisé.",
     'El terminal ha desactivado el\nescudo de energía cercano. Ya no\nse puede usar.',
     "Il terminale ha disattivato lo\nscudo di energia vicino. Non può\npiù essere usato."),
    ('5DE918A079E6F2E9', 'Control terminal has opened',
     'Steuerterminal hat die nahe\nTür geöffnet. Steuerterminal kann\nnicht mehr benutzt werden.',
     "Le terminal de contrôle a ouvert\nla porte voisine. Il ne peut\nplus être utilisé.",
     'El terminal de control ha abierto\nla puerta cercana. Ya no\nse puede usar.',
     "Il terminale di controllo ha aperto\nla porta vicina. Non può\npiù essere usato."),
    ('AA49D0971B2C7C9D', 'Control terminal has opened',
     'Steuerterminal hat die nahe\nTür geöffnet. Terminal kann\nnicht mehr benutzt werden.',
     "Le terminal de contrôle a ouvert\nla porte voisine. Le terminal\nne peut plus être utilisé.",
     'El terminal de control ha abierto\nla puerta cercana. El terminal\nya no se puede usar.',
     "Il terminale di controllo ha aperto\nla porta vicina. Il terminale\nnon può più essere usato."),
]
for _asset, _guard, *_texts in _SCANS:
    for _lang, _text in zip(_LANGS, _texts):
        WHOLE[_lang].append((f'{_asset}#0', _guard, _text))
for _lang, _text in zip(_LANGS, ('SPEZIELLE ZUGANGSCODES:', "CODES D'ACCES SPECIAUX:", 'CÓDIGOS DE ACCESO ESPECIALES:',
                                 'CODICI DI ACCESSO SPECIALI:')):
    SCOPED[_lang].append(('2A617E24A49E0E73#0', 'SPECIAL ACCESS CODES:', _text))
